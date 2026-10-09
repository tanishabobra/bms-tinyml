import scipy.io as sio
import numpy as np
import csv
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
BATTERIES = ["B0005", "B0006", "B0007", "B0018"]
HEALTH_THRESHOLD = 0.80
OUTPUT_CSV = os.path.join(DATA_DIR, "battery_features_v2.csv")


def extract_battery_cycles(battery_name):
    path = os.path.join(DATA_DIR, f"{battery_name}.mat")
    mat = sio.loadmat(path)
    data = mat[battery_name][0, 0]
    cycles = data['cycle'][0]

    discharge_cycles = []
    for c in cycles:
        if c['type'][0] == 'discharge':
            d = c['data'][0, 0]
            if 'Capacity' not in d.dtype.names:
                continue
            discharge_cycles.append(d)

    if not discharge_cycles:
        return []

    initial_capacity = discharge_cycles[0]['Capacity'][0][0]

    rows = []
    for idx, d in enumerate(discharge_cycles):
        voltage = d['Voltage_measured'][0].flatten()
        current = d['Current_measured'][0].flatten()
        temperature = d['Temperature_measured'][0].flatten()
        time = d['Time'][0].flatten()
        capacity = d['Capacity'][0][0]

        if len(voltage) < 3:
            continue

        capacity_fraction = capacity / initial_capacity
        label = 1 if capacity_fraction < HEALTH_THRESHOLD else 0

        slope, _ = np.polyfit(time, voltage, 1)

        mean_current_mag = abs(np.mean(current))
        resistance_proxy = (voltage[0] - voltage[-1]) / mean_current_mag if mean_current_mag > 1e-6 else 0.0

        temperature_rise = temperature[-1] - temperature[0]

        row = {
            "battery": battery_name,
            "cycle_index": idx,
            "voltage_start": voltage[0],
            "voltage_end": voltage[-1],
            "voltage_drop": voltage[0] - voltage[-1],
            "voltage_mean": np.mean(voltage),
            "voltage_min": np.min(voltage),
            "voltage_slope": slope,
            "resistance_proxy": resistance_proxy,
            "temperature_mean": np.mean(temperature),
            "temperature_max": np.max(temperature),
            "temperature_min": np.min(temperature),
            "temperature_rise": temperature_rise,
            "current_mean": np.mean(current),
            "discharge_duration_s": time[-1] - time[0],
            "capacity_ah": capacity,
            "capacity_fraction": capacity_fraction,
            "label_degrading": label,
        }
        rows.append(row)

    return rows


def main():
    all_rows = []
    for battery in BATTERIES:
        print(f"Processing {battery} ...")
        rows = extract_battery_cycles(battery)
        print(f"  -> {len(rows)} discharge cycles extracted")
        all_rows.extend(rows)

    fieldnames = list(all_rows[0].keys())
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(all_rows)

    n_healthy = sum(1 for r in all_rows if r["label_degrading"] == 0)
    n_degrading = sum(1 for r in all_rows if r["label_degrading"] == 1)
    print(f"\nTotal rows: {len(all_rows)}, Healthy: {n_healthy}, Degrading: {n_degrading}")
    print(f"Saved to: {OUTPUT_CSV}")


if __name__ == "__main__":
    main()
