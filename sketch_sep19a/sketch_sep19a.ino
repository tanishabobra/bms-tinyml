#include <SPI.h>
#include <mcp2515.h>

struct can_frame canMsg;
MCP2515 mcp2515(10);  // CS pin

void setup() {
  Serial.begin(115200);

  mcp2515.reset();
  mcp2515.setBitrate(CAN_500KBPS, MCP_8MHZ);  // match STM32's 500kbps
  // NOTE: use MCP_16MHZ instead if your MCP2515 module has a 16MHz crystal
  // (check the crystal on the board -- 8MHz and 16MHz modules both exist)
  mcp2515.setNormalMode();

  Serial.println("CAN receiver ready");
}

void loop() {
  if (mcp2515.readMessage(&canMsg) == MCP2515::ERROR_OK) {
    if (canMsg.can_id == 0x100) {
      int16_t vA_mV = (canMsg.data[0] << 8) | canMsg.data[1];
      int16_t vB_mV = (canMsg.data[2] << 8) | canMsg.data[3];
      int16_t vC_mV = (canMsg.data[4] << 8) | canMsg.data[5];
      int16_t vPack_mV = (canMsg.data[6] << 8) | canMsg.data[7];

      Serial.print("VOLTAGES  A=");
      Serial.print(vA_mV / 1000.0, 3);
      Serial.print("V  B=");
      Serial.print(vB_mV / 1000.0, 3);
      Serial.print("V  C=");
      Serial.print(vC_mV / 1000.0, 3);
      Serial.print("V  Pack=");
      Serial.print(vPack_mV / 1000.0, 3);
      Serial.println("V");
    }
    else if (canMsg.can_id == 0x101) {
      int16_t current_mA = (canMsg.data[0] << 8) | canMsg.data[1];
      int16_t temp_x10 = (canMsg.data[2] << 8) | canMsg.data[3];
      uint8_t soc_pct = canMsg.data[4];
      uint8_t fault_flags = canMsg.data[5];
      uint8_t ml_fault = canMsg.data[6];

      Serial.print("STATUS  Current=");
      Serial.print(current_mA / 1000.0, 3);
      Serial.print("A  Temp=");
      Serial.print(temp_x10 / 10.0, 1);
      Serial.print("C  SoC=");
      Serial.print(soc_pct);
      Serial.print("%  Faults=OV:");
      Serial.print(fault_flags & 0x01);
      Serial.print(" UV:");
      Serial.print((fault_flags >> 1) & 0x01);
      Serial.print(" OCD:");
      Serial.print((fault_flags >> 2) & 0x01);
      Serial.print(" OT:");
      Serial.print((fault_flags >> 3) & 0x01);
      Serial.print(" ANY:");
      Serial.print((fault_flags >> 4) & 0x01);
      Serial.print("  ML_fault=");
      Serial.println(ml_fault);
    }
  }
}
