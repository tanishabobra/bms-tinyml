# Battery Management System with TinyML Fault Prediction on STM32F446RE

Battery packs fail in ways that classical threshold-based protection can't always see coming: a cell degrading over hundreds of cycles looks identical, on any single reading, to a healthy one. This project builds a battery management system from the ground up on an STM32 microcontroller, voltage/current/temperature sensing, debounced relay protection, an Extended Kalman Filter for state-of-charge estimation, a quantized TinyML model trained on real battery degradation data from NASA to predict developing faults before they trip a threshold, and CAN telemetry to get that data off the board, and verifies each subsystem against real hardware and real data rather than assumed correctness.

## Problem Statement

A battery management system has two jobs that pull in different directions. The first is fast and unforgiving: if a cell overcharges, over-discharges, overcurrents, or overheats, the pack has to be disconnected within a bounded time, no exceptions. The second is slow and probabilistic: a cell that's still inside every threshold today can still be measurably degrading, and catching that trend early is the difference between a scheduled replacement and a field failure. Threshold logic alone can't do the second job, by the time a reading crosses a threshold, the fault has already arrived. This project treats those as two separate problems with two separate solutions running side by side on the same hardware, and adds a third layer, state-of-charge estimation, since knowing how full a pack is, is a prerequisite for interpreting almost everything else about it.

## Method

**Hardware.** STM32 Nucleo-F446RE (ARM Cortex-M4), developed in STM32CubeIDE using HAL drivers, C, and FreeRTOS. A 3-cell pack is modeled with a resistor ladder rather than live lithium cells, a controlled electrical stand-in that reproduces the voltage, current, and fault conditions the firmware has to handle, without the safety overhead of cycling real cells during active development.

- 2x ADS1115, 16-bit I2C ADC, cell voltage sensing
- INA219, pack current and voltage, via a 50A/75mV shunt
- NTC thermistor, through a resistor divider into the second ADS1115, for temperature
- Relay, driven through a 2N2222 transistor with a flyback diode, disconnects the load on any fault
- SN65HVD230 CAN transceiver, STM32 side
- Arduino Uno with an MCP2515 CAN module, second bus node

**Protection logic.** Over-voltage, under-voltage, over-current, and over-temperature are each checked every 500ms against fixed thresholds (4.25V, 3.00V, 1.00A, 60°C). A fault must persist for 3 consecutive readings before it trips, and clear for 10 consecutive readings before it resets, debouncing the relay against single noisy samples in either direction without masking a real fault.

**TinyML fault prediction.** A small neural network is trained offline on the NASA Prognostics Center of Excellence (PCoE) Battery Dataset, a public dataset of full charge and discharge cycle histories for a set of lithium-ion cells run to end of life under controlled lab conditions. Four cells from that dataset (B0005, B0006, B0007, B0018) are used here, giving real, physically measured degradation trajectories rather than synthetic or simulated ones. The model learns to classify each discharge cycle as healthy or degrading from 13 features, voltage start/end/drop/mean/min/slope, a resistance proxy, temperature statistics, mean current, and discharge duration. On the device, these are computed as running statistics over a roughly 60-minute window, matching the timescale of the training data, rather than stored per-sample, keeping the memory footprint fixed regardless of window length. The trained model is int8-quantized and deployed on-device via STM32Cube.AI, fitting in 15.5 KiB of flash and about 2 KiB of RAM including the inference runtime, and runs inference directly on the F446RE.

**EKF state-of-charge estimation.** SoC is estimated with an Extended Kalman Filter over a two-state model (SoC, RC-branch polarization voltage), built on a first-order equivalent circuit (R0, R1, C1) fitted from the same NASA PCoE discharge data, and an OCV-SoC lookup table derived from it. Both the filter and the fitting were developed and validated in Python before being ported to C and integrated into firmware.

**CAN telemetry.** Two fixed-format CAN frames (0x100: cell voltages and pack voltage; 0x101: current, temperature, SoC, fault flags, and the TinyML prediction) are transmitted every 500ms from the STM32's CAN1 peripheral. An Arduino running an MCP2515 driver receives and decodes both frames, closing the loop end to end between the two nodes.

**RTOS architecture.** The firmware runs under FreeRTOS as three tasks: a high-priority SensorTask owning the I2C bus and running protection logic, ML window updates, and the EKF predict/update step every 500ms; a low-priority MLInferenceTask, woken by a semaphore only when a full window is ready, keeping the comparatively slow inference call off the time-critical sensor loop; and a CANTask reading a mutex-protected telemetry snapshot and transmitting independently, so a stalled CAN transmission can never delay a fault check.

## Results

| Subsystem | Status | Verification |
|---|---|---|
| Protection logic + relay | Verified | Confirmed live on hardware; correctly latches and clears faults with debounce timing matching spec |
| TinyML fault prediction | Verified | Deployed int8 model: 98.5% accuracy and 100% degrading-cycle recall on held-out cell B0018 (NASA PCoE dataset); 95.6% mean accuracy, 100% mean recall across 4-fold leave-one-battery-out cross-validation; 15.5 KiB flash, about 2 KiB RAM (X-CUBE-AI analysis); confirmed running live inference on-device |
| EKF state-of-charge | Verified | RMSE against coulomb-counted ground truth on a NASA PCoE discharge cycle (B0005), validated in Python: 0.98%, down from 14.88% before a diagnosed and fixed modeling bug, see below; C port confirmed running live on hardware |
| NTC temperature sensing | Verified | Reads approximately 25 to 31°C at room temperature on hardware, matching expected values |
| RTOS task architecture | Verified | All three tasks (SensorTask, MLInferenceTask, CANTask) confirmed running concurrently on hardware with correct mutex/semaphore coordination |
| CAN telemetry | Verified | Frames transmitted by the STM32 confirmed received and correctly decoded by the Arduino node over a live, terminated CAN bus |

The int8 quantization step, somewhat counterintuitively, produced a slightly higher accuracy on the held-out cell (98.5%) than the unquantized float32 model (97.7%), consistent with quantization acting as a mild regularizer here rather than a pure precision loss.

### Bug log

**IR-drop double-counted in the EKF measurement model.** The OCV-SoC lookup table was initially built directly from loaded discharge voltage in the NASA PCoE data, voltage measured while the cell was under a ~2A load, not true open-circuit voltage. Since the EKF's measurement equation separately subtracts `I×R0` and the RC-branch voltage from that same table value, the resistive drop was being subtracted twice: once baked into the table, once again in the filter. The symptom was a SoC estimate that diverged steadily upward over a discharge cycle, RMSE climbing to approximately 15%, rather than the random-walk-around-truth behavior a correctly tuned EKF should show. Diagnosed by tracing the sign and growth pattern of the error back to the measurement residual, not the process model. Fixed by reconstructing the table as a proper pseudo-OCV curve, adding the steady-state `I×(R0+R1)` drop back onto each raw voltage sample before building the lookup table. RMSE dropped to 0.98% immediately after the fix, with no change to the filter's actual gain tuning.

**FreeRTOS include paths not regenerated by CubeMX.** After enabling FreeRTOS in CubeMX on an existing project, the middleware source files were correctly added to disk, but the compiler's include search path in the build settings was not updated to reference them, producing a `FreeRTOS.h: No such file or directory` error that looked like a missing dependency rather than a stale build configuration. Fixed by manually adding the three FreeRTOS include paths to the GCC compiler settings.

## Conclusion

Each subsystem here was built and evaluated against measured behavior: real NASA PCoE discharge data for the TinyML model and EKF, real hardware readings for the sensing and protection logic, and a real second node for CAN telemetry, rather than any of these being assumed correct from the design alone. The EKF bug in particular was a genuine modeling error, not an implementation typo, and tracing it to a specific incorrect assumption, treating loaded voltage as open-circuit voltage, rather than re-tuning the filter's noise parameters to paper over the symptom, was the difference between a 15% and a 1% error.

## Architecture

On the STM32F446RE:

| Peripheral | Function |
|---|---|
| I2C1 | Two ADS1115 ADCs (cell voltages, NTC), INA219 (pack current/voltage) |
| GPIO | Relay driver output |
| CAN1 | Telemetry TX via SN65HVD230, 500kbps |
| FreeRTOS | SensorTask, MLInferenceTask, CANTask |
| X-CUBE-AI | On-device TinyML inference |

Off the board:

An Arduino Uno with an MCP2515 module receives CAN telemetry and decodes both frame types over a Serial Monitor connection, completing the two-node bus.

## Data

NASA Prognostics Center of Excellence (PCoE) Battery Dataset, cells B0005, B0006, B0007, and B0018. Loaded via `scipy.io.loadmat()` in Python for offline feature extraction, model training, and EKF parameter fitting.

## Stack

STM32CubeIDE, C, HAL drivers, FreeRTOS (CMSIS-RTOS v2), STM32Cube.AI/X-CUBE-AI, Python (TensorFlow/Keras, scipy, NumPy) for offline training and EKF validation, Arduino IDE with the autowp MCP2515 library.
