/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (RTOS + CAN)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "network.h"
#include "network_data.h"
#include <math.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

CAN_HandleTypeDef hcan1;

I2C_HandleTypeDef hi2c1;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */

/* --- Shared telemetry struct + RTOS primitives --- */

typedef struct {
  float voltage_cellA, voltage_cellB, voltage_cellC, busVoltage_V;
  float current_A, temperature_C, soc_estimate;
  uint8_t ov_fault, uv_fault, ocd_fault, ot_fault, any_fault;
  uint8_t ml_fault_predicted;
} Telemetry;

static Telemetry telemetry = {0};

static osMutexId_t i2c_mutex;
static osMutexId_t telemetry_mutex;
static osSemaphoreId_t ml_window_ready_sem;

static osThreadId_t sensor_task_handle;
static osThreadId_t ml_task_handle;
static osThreadId_t can_task_handle;

/* --- All I2C-level device functions: unchanged --- */

int16_t ADS1115_ReadDifferential(uint8_t address, uint8_t channelPair)
{
  uint8_t config[3];
  uint8_t data[2];

  uint16_t muxBits = (channelPair == 0) ? 0x0000 : 0x3000;

  uint16_t configValue = 0x8000 | muxBits | 0x0200 | 0x0100 | 0x0080 | 0x0003;

  config[0] = 0x01;
  config[1] = configValue >> 8;
  config[2] = configValue & 0xFF;

  HAL_I2C_Master_Transmit(&hi2c1, address << 1, config, 3, 100);

  uint8_t configReg = 0x01;
  uint8_t configReadBuf[2];
  HAL_I2C_Master_Transmit(&hi2c1, address << 1, &configReg, 1, 100);
  HAL_I2C_Master_Receive(&hi2c1, address << 1, configReadBuf, 2, 100);

  osDelay(10);

  uint8_t reg = 0x00;
  HAL_I2C_Master_Transmit(&hi2c1, address << 1, &reg, 1, 100);
  HAL_I2C_Master_Receive(&hi2c1, address << 1, data, 2, 100);

  return (int16_t)((data[0] << 8) | data[1]);
}

int16_t ADS1115_ReadSingleEnded(uint8_t address, uint8_t channel)
{
  uint8_t config[3];
  uint8_t data[2];

  uint16_t muxBits;
  switch (channel)
  {
    case 0: muxBits = 0x4000; break;
    case 1: muxBits = 0x5000; break;
    case 2: muxBits = 0x6000; break;
    case 3: muxBits = 0x7000; break;
    default: muxBits = 0x4000; break;
  }

  uint16_t configValue = 0x8000 | muxBits | 0x0200 | 0x0100 | 0x0080 | 0x0003;

  config[0] = 0x01;
  config[1] = configValue >> 8;
  config[2] = configValue & 0xFF;

  HAL_I2C_Master_Transmit(&hi2c1, address << 1, config, 3, 100);
  osDelay(10);

  uint8_t reg = 0x00;
  HAL_I2C_Master_Transmit(&hi2c1, address << 1, &reg, 1, 100);
  HAL_I2C_Master_Receive(&hi2c1, address << 1, data, 2, 100);

  return (int16_t)((data[0] << 8) | data[1]);
}

void INA219_Init(void)
{
  uint8_t config[3];
  uint16_t configValue = 0x399F;
  config[0] = 0x00;
  config[1] = configValue >> 8;
  config[2] = configValue & 0xFF;
  HAL_I2C_Master_Transmit(&hi2c1, 0x40 << 1, config, 3, 100);
}

int16_t INA219_ReadShuntVoltage(void)
{
  uint8_t reg = 0x01;
  uint8_t data[2];
  HAL_I2C_Master_Transmit(&hi2c1, 0x40 << 1, &reg, 1, 100);
  HAL_I2C_Master_Receive(&hi2c1, 0x40 << 1, data, 2, 100);
  return (int16_t)((data[0] << 8) | data[1]);
}

int16_t INA219_ReadBusVoltage(void)
{
  uint8_t reg = 0x02;
  uint8_t data[2];
  HAL_I2C_Master_Transmit(&hi2c1, 0x40 << 1, &reg, 1, 100);
  HAL_I2C_Master_Receive(&hi2c1, 0x40 << 1, data, 2, 100);
  uint16_t raw = ((data[0] << 8) | data[1]);
  return (int16_t)(raw >> 3);
}

#define NTC_R_FIXED    10000.0f
#define NTC_R0         10000.0f
#define NTC_T0_KELVIN  298.15f
#define NTC_BETA       3950.0f
#define NTC_SUPPLY_V   3.3f
#define OT_THRESHOLD   60.0f

float NTC_ReadTemperatureC(void)
{
  int16_t raw = ADS1115_ReadSingleEnded(0x49, 1); // confirmed working channel

  float v_midpoint = raw * 4.096f / 32768.0f;

  if (v_midpoint <= 0.01f || v_midpoint >= (NTC_SUPPLY_V - 0.01f))
  {
    return -999.0f;
  }

  float r_ntc = NTC_R_FIXED * v_midpoint / (NTC_SUPPLY_V - v_midpoint);
  float inv_T = (1.0f / NTC_T0_KELVIN) + (1.0f / NTC_BETA) * logf(r_ntc / NTC_R0);
  float T_kelvin = 1.0f / inv_T;
  return T_kelvin - 273.15f;
}

/* --- Protection logic: unchanged --- */

typedef struct {
  uint8_t ov_fault, uv_fault, ocd_fault, ot_fault, any_fault;
} FaultStatus;

static FaultStatus faults = {0, 0, 0, 0, 0};

#define OV_THRESHOLD  4.25f
#define UV_THRESHOLD  3.00f
#define OCD_THRESHOLD 1.00f
#define FAULT_TRIP_COUNT    3
#define FAULT_CLEAR_COUNT   10

static uint8_t fault_debounce_counter = 0;
static uint8_t clear_debounce_counter = 0;

void CheckProtection(float vA, float vB, float vC, float current, float temperature)
{
  uint8_t ov_now  = (vA > OV_THRESHOLD) || (vB > OV_THRESHOLD) || (vC > OV_THRESHOLD);
  uint8_t uv_now  = (vA < UV_THRESHOLD) || (vB < UV_THRESHOLD) || (vC < UV_THRESHOLD);
  uint8_t ocd_now = (current > OCD_THRESHOLD);
  uint8_t ot_now  = (temperature > -900.0f) && (temperature > OT_THRESHOLD);
  uint8_t any_now = ov_now || uv_now || ocd_now || ot_now;

  if (any_now)
  {
    if (fault_debounce_counter < 255) fault_debounce_counter++;
    clear_debounce_counter = 0;

    if (fault_debounce_counter >= FAULT_TRIP_COUNT)
    {
      faults.ov_fault  = ov_now;
      faults.uv_fault  = uv_now;
      faults.ocd_fault = ocd_now;
      faults.ot_fault  = ot_now;
      faults.any_fault = 1;
    }
  }
  else
  {
    if (clear_debounce_counter < 255) clear_debounce_counter++;
    fault_debounce_counter = 0;

    if (faults.any_fault && clear_debounce_counter >= FAULT_CLEAR_COUNT)
    {
      faults.ov_fault = 0;
      faults.uv_fault = 0;
      faults.ocd_fault = 0;
      faults.ot_fault = 0;
      faults.any_fault = 0;
    }
  }
}

/* --- ML window + inference: unchanged --- */

#define ML_WINDOW_DURATION_MS (60UL * 60UL * 1000UL)

typedef struct {
  uint8_t initialized;
  uint32_t window_start_ms;
  float v_start, v_end, v_min, v_max, v_sum;
  float t_start, t_end, t_min, t_max, t_sum;
  float i_sum;
  uint32_t sample_count;
} MLWindow;

static MLWindow ml_window = {0};
static uint8_t ml_fault_predicted = 0;

AI_ALIGNED(4) static ai_u8 ml_activations[AI_NETWORK_DATA_ACTIVATIONS_SIZE];
static ai_handle ml_network = AI_HANDLE_NULL;
static ai_buffer *ml_ai_input;
static ai_buffer *ml_ai_output;
AI_ALIGNED(4) static ai_i8 ml_in_data[AI_NETWORK_IN_1_SIZE_BYTES];
AI_ALIGNED(4) static ai_i8 ml_out_data[AI_NETWORK_OUT_1_SIZE_BYTES];

#define ML_INPUT_SCALE       0.03585321083664894f
#define ML_INPUT_ZERO_POINT  (-22)
#define ML_OUTPUT_SCALE      0.00390625f
#define ML_OUTPUT_ZERO_POINT (-128)

static const float ml_feature_mean[13] = {
  4.192808955132895f, 3.272409089583816f, 0.9203998655490792f,
  3.4993529584264813f, 2.381492897664002f, -0.0002247136744084125f,
  0.49307353410537685f, 32.650578043119715f, 40.05161188619845f,
  24.118333404703275f, 12.608154840005213f, -1.833948924395799f,
  3129.523886904762f
};
static const float ml_feature_scale[13] = {
  0.00931527534691189f, 0.4221091341784416f, 0.42299735424333007f,
  0.05079193023033349f, 0.24488864532021887f, 4.297661349659307e-05f,
  0.19936359353370495f, 0.8018611177388473f, 1.1842801512585357f,
  0.4721036585599512f, 2.1909839096404036f, 0.11241392866188613f,
  245.91931258935674f
};

void ML_WindowUpdate(float voltage, float temperature, float current, uint32_t time_ms)
{
  if (!ml_window.initialized)
  {
    ml_window.window_start_ms = time_ms;
    ml_window.v_start = voltage;
    ml_window.v_min = voltage;
    ml_window.v_max = voltage;
    ml_window.v_sum = 0.0f;
    ml_window.t_start = temperature;
    ml_window.t_min = temperature;
    ml_window.t_max = temperature;
    ml_window.t_sum = 0.0f;
    ml_window.i_sum = 0.0f;
    ml_window.sample_count = 0;
    ml_window.initialized = 1;
  }

  ml_window.v_end = voltage;
  ml_window.v_sum += voltage;
  if (voltage < ml_window.v_min) ml_window.v_min = voltage;
  if (voltage > ml_window.v_max) ml_window.v_max = voltage;

  ml_window.t_end = temperature;
  ml_window.t_sum += temperature;
  if (temperature < ml_window.t_min) ml_window.t_min = temperature;
  if (temperature > ml_window.t_max) ml_window.t_max = temperature;

  ml_window.i_sum += current;
  ml_window.sample_count++;
}

uint8_t ML_WindowReady(uint32_t time_ms)
{
  if (!ml_window.initialized || ml_window.sample_count < 10) return 0;
  return (time_ms - ml_window.window_start_ms) >= ML_WINDOW_DURATION_MS;
}

void ML_ComputeFeatures(float *out_features, uint32_t time_ms)
{
  float n = (float)ml_window.sample_count;
  float v_mean = ml_window.v_sum / n;
  float t_mean = ml_window.t_sum / n;
  float i_mean = ml_window.i_sum / n;

  float duration_s = (time_ms - ml_window.window_start_ms) / 1000.0f;
  if (duration_s < 0.001f) duration_s = 0.001f;

  float v_slope = (ml_window.v_end - ml_window.v_start) / duration_s;

  float i_mean_mag = (i_mean < 0) ? -i_mean : i_mean;
  float resistance_proxy = (i_mean_mag > 1e-6f)
    ? (ml_window.v_start - ml_window.v_end) / i_mean_mag
    : 0.0f;

  float t_rise = ml_window.t_end - ml_window.t_start;

  out_features[0] = ml_window.v_start;
  out_features[1] = ml_window.v_end;
  out_features[2] = ml_window.v_start - ml_window.v_end;
  out_features[3] = v_mean;
  out_features[4] = ml_window.v_min;
  out_features[5] = v_slope;
  out_features[6] = resistance_proxy;
  out_features[7] = t_mean;
  out_features[8] = ml_window.t_max;
  out_features[9] = ml_window.t_min;
  out_features[10] = t_rise;
  out_features[11] = i_mean;
  out_features[12] = duration_s;
}

void ML_WindowReset(void)
{
  ml_window.initialized = 0;
}

int ML_NetworkInit(void)
{
  ai_error err;
  const ai_handle act_addr[] = { ml_activations };
  err = ai_network_create_and_init(&ml_network, act_addr, NULL);
  if (err.type != AI_ERROR_NONE) return -1;
  ml_ai_input = ai_network_inputs_get(ml_network, NULL);
  ml_ai_output = ai_network_outputs_get(ml_network, NULL);
  return 0;
}

uint8_t ML_RunInference(uint32_t time_ms)
{
  float features[13];
  ML_ComputeFeatures(features, time_ms);

  for (int i = 0; i < 13; i++)
  {
    float normalized = (features[i] - ml_feature_mean[i]) / ml_feature_scale[i];
    int32_t quantized = (int32_t)(normalized / ML_INPUT_SCALE + ML_INPUT_ZERO_POINT + 0.5f);
    if (quantized > 127) quantized = 127;
    if (quantized < -128) quantized = -128;
    ml_in_data[i] = (ai_i8)quantized;
  }

  ml_ai_input[0].data = AI_HANDLE_PTR(ml_in_data);
  ml_ai_output[0].data = AI_HANDLE_PTR(ml_out_data);

  ai_i32 batch = ai_network_run(ml_network, ml_ai_input, ml_ai_output);
  if (batch != 1) return 0;

  float output_prob = (ml_out_data[0] - ML_OUTPUT_ZERO_POINT) * ML_OUTPUT_SCALE;
  return (output_prob > 0.5f) ? 1 : 0;
}

/* --- EKF: unchanged --- */

#define ECM_R0   0.1073f
#define ECM_R1   0.0861f
#define ECM_TAU  163.6745f
#define EKF_CAPACITY_AH 1.8565f  // TODO: replace with your pack's rated Ah

#define OCV_TABLE_SIZE 21

static const float ocv_soc_grid[OCV_TABLE_SIZE] = {
  0.00f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f,
  0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f
};

static const float ocv_voltage_table[OCV_TABLE_SIZE] = {
  3.4020f, 3.5955f, 3.7331f, 3.7850f, 3.8160f, 3.8396f, 3.8583f, 3.8755f,
  3.8932f, 3.9131f, 3.9361f, 3.9620f, 3.9895f, 4.0186f, 4.0501f, 4.0839f,
  4.1201f, 4.1598f, 4.2043f, 4.2596f, 4.2596f
};

typedef struct { float soc; float v1; float P[2][2]; } EKF_State;

static EKF_State ekf = {
  .soc = 1.0f, .v1 = 0.0f,
  .P = {{1e-4f, 0.0f}, {0.0f, 1e-4f}}
};

static const float EKF_Q_SOC = 1e-8f;
static const float EKF_Q_V1  = 1e-6f;
static const float EKF_R_NOISE = 1e-4f;

static float EKF_OCV(float soc)
{
  if (soc <= ocv_soc_grid[0]) return ocv_voltage_table[0];
  if (soc >= ocv_soc_grid[OCV_TABLE_SIZE - 1]) return ocv_voltage_table[OCV_TABLE_SIZE - 1];
  for (int i = 0; i < OCV_TABLE_SIZE - 1; i++)
  {
    if (soc >= ocv_soc_grid[i] && soc <= ocv_soc_grid[i + 1])
    {
      float frac = (soc - ocv_soc_grid[i]) / (ocv_soc_grid[i + 1] - ocv_soc_grid[i]);
      return ocv_voltage_table[i] + frac * (ocv_voltage_table[i + 1] - ocv_voltage_table[i]);
    }
  }
  return ocv_voltage_table[OCV_TABLE_SIZE - 1];
}

static float EKF_dOCV_dSoC(float soc)
{
  const float eps = 0.001f;
  float soc_hi = soc + eps;
  float soc_lo = soc - eps;
  if (soc_hi > 1.0f) soc_hi = 1.0f;
  if (soc_lo < 0.0f) soc_lo = 0.0f;
  return (EKF_OCV(soc_hi) - EKF_OCV(soc_lo)) / (soc_hi - soc_lo);
}

void EKF_Init(float initial_soc)
{
  ekf.soc = initial_soc;
  ekf.v1 = 0.0f;
  ekf.P[0][0] = 1e-4f; ekf.P[0][1] = 0.0f;
  ekf.P[1][0] = 0.0f;  ekf.P[1][1] = 1e-4f;
}

void EKF_Predict(float current, float dt)
{
  float exp_term = expf(-dt / ECM_TAU);
  float soc_new = ekf.soc - (dt / (EKF_CAPACITY_AH * 3600.0f)) * current;
  float v1_new  = ekf.v1 * exp_term + ECM_R1 * (1.0f - exp_term) * current;

  ekf.soc = soc_new;
  ekf.v1 = v1_new;

  float P00 = ekf.P[0][0], P01 = ekf.P[0][1], P10 = ekf.P[1][0], P11 = ekf.P[1][1];
  ekf.P[0][0] = P00 + EKF_Q_SOC;
  ekf.P[0][1] = P01 * exp_term;
  ekf.P[1][0] = P10 * exp_term;
  ekf.P[1][1] = P11 * exp_term * exp_term + EKF_Q_V1;
}

void EKF_Update(float measured_voltage, float current)
{
  float predicted_voltage = EKF_OCV(ekf.soc) - ekf.v1 - current * ECM_R0;
  float residual = measured_voltage - predicted_voltage;

  float H0 = EKF_dOCV_dSoC(ekf.soc);
  float H1 = -1.0f;

  float S = H0 * (H0 * ekf.P[0][0] + H1 * ekf.P[1][0])
          + H1 * (H0 * ekf.P[0][1] + H1 * ekf.P[1][1])
          + EKF_R_NOISE;

  float K0 = (ekf.P[0][0] * H0 + ekf.P[0][1] * H1) / S;
  float K1 = (ekf.P[1][0] * H0 + ekf.P[1][1] * H1) / S;

  ekf.soc += K0 * residual;
  ekf.v1  += K1 * residual;

  if (ekf.soc < 0.0f) ekf.soc = 0.0f;
  if (ekf.soc > 1.0f) ekf.soc = 1.0f;

  float P00 = ekf.P[0][0], P01 = ekf.P[0][1], P10 = ekf.P[1][0], P11 = ekf.P[1][1];
  ekf.P[0][0] = (1.0f - K0 * H0) * P00 - K0 * H1 * P10;
  ekf.P[0][1] = (1.0f - K0 * H0) * P01 - K0 * H1 * P11;
  ekf.P[1][0] = -K1 * H0 * P00 + (1.0f - K1 * H1) * P10;
  ekf.P[1][1] = -K1 * H0 * P01 + (1.0f - K1 * H1) * P11;
}

float EKF_GetSoC(void) { return ekf.soc; }

/* --- CAN: telemetry init + send functions --- */

#define CAN_ID_VOLTAGES  0x100
#define CAN_ID_STATUS    0x101

static CAN_TxHeaderTypeDef can_tx_header;
static uint32_t can_tx_mailbox;

int CAN_TelemetryInit(void)
{
  if (HAL_CAN_Start(&hcan1) != HAL_OK) return -1;
  can_tx_header.IDE = CAN_ID_STD;
  can_tx_header.RTR = CAN_RTR_DATA;
  can_tx_header.DLC = 8;
  can_tx_header.TransmitGlobalTime = DISABLE;
  return 0;
}

int CAN_SendVoltages(float vA, float vB, float vC, float vPack)
{
  uint8_t data[8];
  int16_t vA_mV = (int16_t)(vA * 1000.0f);
  int16_t vB_mV = (int16_t)(vB * 1000.0f);
  int16_t vC_mV = (int16_t)(vC * 1000.0f);
  int16_t vPack_mV = (int16_t)(vPack * 1000.0f);

  data[0] = (vA_mV >> 8) & 0xFF; data[1] = vA_mV & 0xFF;
  data[2] = (vB_mV >> 8) & 0xFF; data[3] = vB_mV & 0xFF;
  data[4] = (vC_mV >> 8) & 0xFF; data[5] = vC_mV & 0xFF;
  data[6] = (vPack_mV >> 8) & 0xFF; data[7] = vPack_mV & 0xFF;

  can_tx_header.StdId = CAN_ID_VOLTAGES;
  if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0) return -1;
  return HAL_CAN_AddTxMessage(&hcan1, &can_tx_header, data, &can_tx_mailbox) == HAL_OK ? 0 : -1;
}

int CAN_SendStatus(float current, float temperature, float soc,
                    uint8_t ov, uint8_t uv, uint8_t ocd, uint8_t ot,
                    uint8_t any_fault, uint8_t ml_fault)
{
  uint8_t data[8];
  int16_t current_mA = (int16_t)(current * 1000.0f);
  int16_t temp_x10 = (int16_t)(temperature * 10.0f);
  uint8_t soc_pct = (uint8_t)(soc * 100.0f);

  uint8_t fault_flags = (ov & 0x01) | ((uv & 0x01) << 1) | ((ocd & 0x01) << 2)
                       | ((ot & 0x01) << 3) | ((any_fault & 0x01) << 4);

  data[0] = (current_mA >> 8) & 0xFF; data[1] = current_mA & 0xFF;
  data[2] = (temp_x10 >> 8) & 0xFF;   data[3] = temp_x10 & 0xFF;
  data[4] = soc_pct;
  data[5] = fault_flags;
  data[6] = ml_fault;
  data[7] = 0x00;

  can_tx_header.StdId = CAN_ID_STATUS;
  if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0) return -1;
  return HAL_CAN_AddTxMessage(&hcan1, &can_tx_header, data, &can_tx_mailbox) == HAL_OK ? 0 : -1;
}

/* ============================================================
   RTOS TASKS
   ============================================================ */

static void SensorTask(void *argument)
{
  static uint32_t last_ekf_tick = 0;

  for (;;)
  {
    osMutexAcquire(i2c_mutex, osWaitForever);

    int16_t raw_cellB = ADS1115_ReadDifferential(0x48, 0);
    int16_t raw_cellC = ADS1115_ReadDifferential(0x48, 3);
    int16_t raw_cellA = ADS1115_ReadDifferential(0x49, 3);

    float voltage_cellA = (raw_cellA * 4.096f / 32768.0f) * 4.134f;
    float voltage_cellB = (raw_cellB * 4.096f / 32768.0f) * 4.134f;
    float voltage_cellC = (raw_cellC * 4.096f / 32768.0f) * 4.134f;

    int16_t raw_shuntVoltage = INA219_ReadShuntVoltage();
    float shuntVoltage_mV = raw_shuntVoltage * 0.01f;
    float current_A = shuntVoltage_mV / 1000.0f / 0.0015f;

    int16_t busVoltage_raw = INA219_ReadBusVoltage();
    float busVoltage_V = busVoltage_raw * 0.004f;

    float temperature_C = NTC_ReadTemperatureC();

    osMutexRelease(i2c_mutex);

    CheckProtection(voltage_cellA, voltage_cellB, voltage_cellC, current_A, temperature_C);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, faults.any_fault ? GPIO_PIN_RESET : GPIO_PIN_SET);

    uint32_t now_ms = HAL_GetTick();
    ML_WindowUpdate(voltage_cellB, temperature_C, current_A, now_ms);

    if (ML_WindowReady(now_ms))
    {
      osSemaphoreRelease(ml_window_ready_sem);
    }

    float dt_s = (now_ms - last_ekf_tick) / 1000.0f;
    if (dt_s > 0.001f)
    {
      EKF_Predict(current_A, dt_s);
      EKF_Update(voltage_cellB, current_A);
    }
    last_ekf_tick = now_ms;

    float soc_estimate = EKF_GetSoC();

    osMutexAcquire(telemetry_mutex, osWaitForever);
    telemetry.voltage_cellA = voltage_cellA;
    telemetry.voltage_cellB = voltage_cellB;
    telemetry.voltage_cellC = voltage_cellC;
    telemetry.busVoltage_V = busVoltage_V;
    telemetry.current_A = current_A;
    telemetry.temperature_C = temperature_C;
    telemetry.soc_estimate = soc_estimate;
    telemetry.ov_fault = faults.ov_fault;
    telemetry.uv_fault = faults.uv_fault;
    telemetry.ocd_fault = faults.ocd_fault;
    telemetry.ot_fault = faults.ot_fault;
    telemetry.any_fault = faults.any_fault;
    telemetry.ml_fault_predicted = ml_fault_predicted;
    osMutexRelease(telemetry_mutex);

    osDelay(500);
  }
}

static void MLInferenceTask(void *argument)
{
  for (;;)
  {
    osSemaphoreAcquire(ml_window_ready_sem, osWaitForever);

    uint32_t now_ms = HAL_GetTick();
    ml_fault_predicted = ML_RunInference(now_ms);
    ML_WindowReset();
  }
}

static void CANTask(void *argument)
{
  for (;;)
  {
    Telemetry snapshot;

    osMutexAcquire(telemetry_mutex, osWaitForever);
    snapshot = telemetry;
    osMutexRelease(telemetry_mutex);

    CAN_SendVoltages(snapshot.voltage_cellA, snapshot.voltage_cellB,
                      snapshot.voltage_cellC, snapshot.busVoltage_V);
    CAN_SendStatus(snapshot.current_A, snapshot.temperature_C, snapshot.soc_estimate,
                   snapshot.ov_fault, snapshot.uv_fault, snapshot.ocd_fault,
                   snapshot.ot_fault, snapshot.any_fault, snapshot.ml_fault_predicted);

    osDelay(500);
  }
}

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_CAN1_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_CAN1_Init();
  /* USER CODE BEGIN 2 */
  INA219_Init();

  if (ML_NetworkInit() != 0)
  {
    Error_Handler();
  }

  EKF_Init(1.0f); // TODO: replace with a real startup SoC estimate if known

  if (CAN_TelemetryInit() != 0)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  i2c_mutex = osMutexNew(NULL);
  telemetry_mutex = osMutexNew(NULL);
  if (i2c_mutex == NULL || telemetry_mutex == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  ml_window_ready_sem = osSemaphoreNew(1, 0, NULL);
  if (ml_window_ready_sem == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  const osThreadAttr_t sensor_task_attr = {
    .name = "SensorTask",
    .priority = osPriorityHigh,
    .stack_size = 1024
  };
  const osThreadAttr_t ml_task_attr = {
    .name = "MLInferenceTask",
    .priority = osPriorityLow,
    .stack_size = 1024
  };
  const osThreadAttr_t can_task_attr = {
    .name = "CANTask",
    .priority = osPriorityNormal,
    .stack_size = 512
  };

  sensor_task_handle = osThreadNew(SensorTask, NULL, &sensor_task_attr);
  ml_task_handle = osThreadNew(MLInferenceTask, NULL, &ml_task_attr);
  can_task_handle = osThreadNew(CANTask, NULL, &can_task_attr);

  if (sensor_task_handle == NULL || ml_task_handle == NULL || can_task_handle == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Initialize leds */
  BSP_LED_Init(LED2);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN1_Init(void)
{

  /* USER CODE BEGIN CAN1_Init 0 */

  /* USER CODE END CAN1_Init 0 */

  /* USER CODE BEGIN CAN1_Init 1 */

  /* USER CODE END CAN1_Init 1 */
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 30;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_1TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_1TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = DISABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = DISABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN1_Init 2 */

  /* USER CODE END CAN1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pins : USART_TX_Pin USART_RX_Pin */
  GPIO_InitStruct.Pin = USART_TX_Pin|USART_RX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
