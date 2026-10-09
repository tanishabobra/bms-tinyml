import scipy.io as sio
import numpy as np
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
BATTERY = "B0005"
CYCLE_INDEX = 0

mat = sio.loadmat(os.path.join(DATA_DIR, f"{BATTERY}.mat"))
data = mat[BATTERY][0, 0]
cycles = data['cycle'][0]

discharge_cycles = [c for c in cycles if c['type'][0] == 'discharge']
d = discharge_cycles[CYCLE_INDEX]['data'][0, 0]

voltage = d['Voltage_measured'][0].flatten()
current = d['Current_measured'][0].flatten()
time = d['Time'][0].flatten()
capacity_ah = d['Capacity'][0][0]

dt = np.diff(time, prepend=time[0])
charge_removed_ah = np.cumsum(-current * dt) / 3600.0

soc = 1.0 - (charge_removed_ah / capacity_ah)
soc = np.clip(soc, 0.0, 1.0)

print(f"Battery: {BATTERY}, Cycle index: {CYCLE_INDEX}")
print(f"Total capacity this cycle: {capacity_ah:.4f} Ah")
print(f"Number of samples: {len(voltage)}")
print(f"\nSoC range: {soc.min():.3f} to {soc.max():.3f}")
print(f"Voltage range: {voltage.min():.3f}V to {voltage.max():.3f}V")

print("\n--- OCV-SoC table (sampled every 10th point) ---")
print(f"{'SoC':>8} {'Voltage':>10}")
for i in range(0, len(soc), max(1, len(soc)//20)):
    print(f"{soc[i]:8.3f} {voltage[i]:10.4f}")

output_path = os.path.join(DATA_DIR, "ocv_soc_table.csv")
with open(output_path, "w") as f:
    f.write("soc,voltage,current,time\n")
    for i in range(len(soc)):
        f.write(f"{soc[i]:.6f},{voltage[i]:.6f},{current[i]:.6f},{time[i]:.6f}\n")

print(f"\nFull table saved to: {output_path}")
