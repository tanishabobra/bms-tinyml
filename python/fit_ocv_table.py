import numpy as np
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
INPUT_CSV = os.path.join(DATA_DIR, "ocv_soc_table.csv")

data = np.genfromtxt(INPUT_CSV, delimiter=",", skip_header=1)
soc_raw = data[:, 0]
voltage_raw = data[:, 1]
current_raw = data[:, 2]

R0 = 0.1073
R1 = 0.0861
voltage_raw = voltage_raw + np.abs(current_raw) * (R0 + R1)

sort_idx = np.argsort(soc_raw)
soc_sorted = soc_raw[sort_idx]
voltage_sorted = voltage_raw[sort_idx]

SOC_FLOOR = 0.02
keep_mask = soc_sorted >= SOC_FLOOR
soc_sorted = soc_sorted[keep_mask]
voltage_sorted = voltage_sorted[keep_mask]

_, unique_idx = np.unique(soc_sorted, return_index=True)
soc_clean = soc_sorted[unique_idx]
voltage_clean = voltage_sorted[unique_idx]

soc_grid = np.linspace(0.0, 1.0, 21)
voltage_grid = np.interp(soc_grid, soc_clean, voltage_clean)

for i in range(1, len(voltage_grid)):
    if voltage_grid[i] < voltage_grid[i - 1]:
        voltage_grid[i] = voltage_grid[i - 1]

print("--- Corrected (pseudo-OCV) lookup table ---")
print(f"{'SoC':>6} {'Voltage':>10}")
for s, v in zip(soc_grid, voltage_grid):
    print(f"{s:6.2f} {v:10.4f}")

diffs = np.diff(voltage_grid)
if np.all(diffs >= 0):
    print("\n[OK] Table is monotonically non-decreasing with SoC.")
else:
    print(f"\n[WARNING] Non-monotonic points at indices: {np.where(diffs < 0)[0]}")
