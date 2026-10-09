import scipy.io as sio
import numpy as np
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
BATTERY = "B0005"
CYCLE_INDEX = 0

R0 = 0.1073
R1 = 0.0861
C1 = 1901.46
TAU = R1 * C1

OCV_SOC_GRID = np.array([0.00, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40,
                          0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85,
                          0.90, 0.95, 1.00])
OCV_VOLTAGE_TABLE = np.array([3.4020, 3.5955, 3.7331, 3.7850, 3.8160, 3.8396,
                               3.8583, 3.8755, 3.8932, 3.9131, 3.9361, 3.9620,
                               3.9895, 4.0186, 4.0501, 4.0839, 4.1201, 4.1598,
                               4.2043, 4.2596, 4.2596])

def ocv(soc):
    soc_clamped = np.clip(soc, 0.0, 1.0)
    return np.interp(soc_clamped, OCV_SOC_GRID, OCV_VOLTAGE_TABLE)

def docv_dsoc(soc):
    eps = 0.001
    return (ocv(soc + eps) - ocv(soc - eps)) / (2 * eps)


class EKF_SoC:
    def __init__(self, capacity_ah, initial_soc=1.0):
        self.Q_capacity = capacity_ah
        self.x = np.array([initial_soc, 0.0])
        self.P = np.diag([1e-4, 1e-4])
        self.Q_noise = np.diag([1e-8, 1e-6])
        self.R_noise = np.array([[1e-4]])

    def predict(self, current, dt):
        soc, v1 = self.x
        soc_new = soc - (dt / (self.Q_capacity * 3600.0)) * current
        v1_new = v1 * np.exp(-dt / TAU) + R1 * (1 - np.exp(-dt / TAU)) * current
        self.x = np.array([soc_new, v1_new])
        F = np.array([[1.0, 0.0], [0.0, np.exp(-dt / TAU)]])
        self.P = F @ self.P @ F.T + self.Q_noise

    def update(self, measured_voltage, current):
        soc, v1 = self.x
        predicted_voltage = ocv(soc) - v1 - current * R0
        residual = measured_voltage - predicted_voltage
        H = np.array([[docv_dsoc(soc), -1.0]])
        S = H @ self.P @ H.T + self.R_noise
        K = self.P @ H.T @ np.linalg.inv(S)
        self.x = self.x + (K.flatten() * residual)
        self.P = (np.eye(2) - K @ H) @ self.P

    def get_soc(self):
        return self.x[0]


mat = sio.loadmat(os.path.join(DATA_DIR, f"{BATTERY}.mat"))
data = mat[BATTERY][0, 0]
cycles = data['cycle'][0]
discharge_cycles = [c for c in cycles if c['type'][0] == 'discharge']
d = discharge_cycles[CYCLE_INDEX]['data'][0, 0]

voltage = d['Voltage_measured'][0].flatten()
current = d['Current_measured'][0].flatten()
time = d['Time'][0].flatten()
capacity_ah = d['Capacity'][0][0]

dt_array = np.diff(time, prepend=time[0])
charge_removed_ah = np.cumsum(-current * dt_array) / 3600.0
true_soc = np.clip(1.0 - (charge_removed_ah / capacity_ah), 0.0, 1.0)

ekf = EKF_SoC(capacity_ah=capacity_ah, initial_soc=1.0)
ekf_soc_history = []

for i in range(len(voltage)):
    if i > 0:
        dt = time[i] - time[i - 1]
        ekf.predict(current=-current[i], dt=dt)
    ekf.update(measured_voltage=voltage[i], current=-current[i])
    ekf_soc_history.append(ekf.get_soc())

ekf_soc_history = np.array(ekf_soc_history)
error = ekf_soc_history - true_soc
rmse = np.sqrt(np.mean(error**2))
max_error = np.max(np.abs(error))

print(f"Battery: {BATTERY}, Cycle: {CYCLE_INDEX}")
print(f"Samples: {len(voltage)}")
print(f"\n--- EKF vs Coulomb-Counting Ground Truth ---")
print(f"RMSE: {rmse:.4f} ({rmse*100:.2f}% SoC)")
print(f"Max absolute error: {max_error:.4f} ({max_error*100:.2f}% SoC)")

print(f"\n{'idx':>4} {'time(s)':>8} {'true_soc':>9} {'ekf_soc':>9} {'error':>8}")
for i in range(0, len(voltage), max(1, len(voltage)//20)):
    print(f"{i:4d} {time[i]:8.1f} {true_soc[i]:9.4f} {ekf_soc_history[i]:9.4f} {error[i]:8.4f}")
