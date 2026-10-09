import scipy.io as sio
import numpy as np
from scipy.optimize import curve_fit
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

print("First 10 samples:")
print(f"{'idx':>4} {'time(s)':>8} {'voltage(V)':>10} {'current(A)':>10}")
for i in range(10):
    print(f"{i:4d} {time[i]:8.2f} {voltage[i]:10.4f} {current[i]:10.4f}")

STEP_THRESHOLD = 0.5
step_idx = np.argmax(np.abs(current) > STEP_THRESHOLD)

print(f"\nCurrent step detected at index {step_idx} (time={time[step_idx]:.2f}s)")
print(f"Voltage just before step: {voltage[step_idx-1]:.4f}V")
print(f"Voltage just after step:  {voltage[step_idx]:.4f}V")

I_load = abs(current[step_idx])
V_before = voltage[step_idx - 1]
V_immediately_after = voltage[step_idx]

R0 = (V_before - V_immediately_after) / I_load
print(f"\nLoad current: {I_load:.4f} A")
print(f"R0 (ohmic resistance) = {R0:.4f} ohm")

FIT_WINDOW = 15
fit_start = step_idx
fit_end = min(step_idx + FIT_WINDOW, len(voltage))

t_fit = time[fit_start:fit_end] - time[fit_start]
v_fit = voltage[fit_start:fit_end]

def relaxation_model(t, v_inf, v0, tau):
    return v_inf + (v0 - v_inf) * np.exp(-t / tau)

p0 = [v_fit[-1], v_fit[0], 5.0]
popt, _ = curve_fit(relaxation_model, t_fit, v_fit, p0=p0, maxfev=5000)
v_inf_fit, v0_fit, tau_fit = popt

print(f"\n--- RC relaxation fit ---")
print(f"Fit window: {FIT_WINDOW} samples after step ({t_fit[-1]:.2f}s span)")
print(f"V0 (immediately after step):  {v0_fit:.4f} V")
print(f"V_inf (settled, this window): {v_inf_fit:.4f} V")
print(f"Time constant tau = R1*C1:    {tau_fit:.4f} s")

R1 = abs(v0_fit - v_inf_fit) / I_load
C1 = tau_fit / R1

print(f"\nR1 (polarization resistance) = {R1:.4f} ohm")
print(f"C1 (polarization capacitance) = {C1:.2f} F")

print(f"\n--- Summary: 1st-order RC equivalent circuit parameters ---")
print(f"R0 = {R0:.4f} ohm")
print(f"R1 = {R1:.4f} ohm")
print(f"C1 = {C1:.2f} F")
print(f"tau (R1*C1) = {tau_fit:.4f} s")

output_path = os.path.join(DATA_DIR, "ecm_params.txt")
with open(output_path, "w") as f:
    f.write(f"R0 = {R0:.6f}\n")
    f.write(f"R1 = {R1:.6f}\n")
    f.write(f"C1 = {C1:.6f}\n")
    f.write(f"tau = {tau_fit:.6f}\n")
print(f"\nParameters saved to: {output_path}")
