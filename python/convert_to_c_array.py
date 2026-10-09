import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
TFLITE_PATH = os.path.join(DATA_DIR, "battery_health_int8.tflite")
OUTPUT_HEADER = os.path.join(DATA_DIR, "battery_health_model_data.h")

with open(TFLITE_PATH, "rb") as f:
    model_bytes = f.read()

array_name = "g_battery_health_model_data"

with open(OUTPUT_HEADER, "w") as f:
    f.write("// Auto-generated from battery_health_int8.tflite\n")
    f.write("// Do not edit by hand -- regenerate from convert_to_c_array.py if the model changes\n\n")
    f.write("#ifndef BATTERY_HEALTH_MODEL_DATA_H\n")
    f.write("#define BATTERY_HEALTH_MODEL_DATA_H\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"alignas(8) const unsigned char {array_name}[] = {{\n")

    for i in range(0, len(model_bytes), 12):
        chunk = model_bytes[i:i+12]
        hex_values = ", ".join(f"0x{b:02x}" for b in chunk)
        f.write(f"  {hex_values},\n")

    f.write("};\n\n")
    f.write(f"const unsigned int {array_name}_len = {len(model_bytes)};\n\n")
    f.write("#endif // BATTERY_HEALTH_MODEL_DATA_H\n")

print(f"C header written to: {OUTPUT_HEADER}")
print(f"Model size: {len(model_bytes)} bytes")
print(f"Array name: {array_name}")
print(f"Length variable: {array_name}_len")
