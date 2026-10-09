import tensorflow as tf
import pandas as pd
import numpy as np
import json
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
MODEL_PATH = os.path.join(DATA_DIR, "battery_health_model_final.keras")
CSV_PATH = os.path.join(DATA_DIR, "battery_features_v2.csv")
SCALER_PATH = os.path.join(DATA_DIR, "scaler_params.json")

FEATURE_COLS = [
    "voltage_start", "voltage_end", "voltage_drop", "voltage_mean", "voltage_min",
    "voltage_slope", "resistance_proxy",
    "temperature_mean", "temperature_max", "temperature_min", "temperature_rise",
    "current_mean", "discharge_duration_s",
]
LABEL_COL = "label_degrading"

model = tf.keras.models.load_model(MODEL_PATH)

with open(SCALER_PATH) as f:
    scaler_params = json.load(f)
mean = np.array(scaler_params["mean"])
scale = np.array(scaler_params["scale"])

df = pd.read_csv(CSV_PATH)
train_df = df[df["battery"].isin(["B0005", "B0006", "B0007"])]
test_df = df[df["battery"] == "B0018"]

X_train_raw = train_df[FEATURE_COLS].values
X_test_raw = test_df[FEATURE_COLS].values
y_test = test_df[LABEL_COL].values

X_train_scaled = (X_train_raw - mean) / scale
X_test_scaled = (X_test_raw - mean) / scale

converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_float_model = converter.convert()

float_path = os.path.join(DATA_DIR, "battery_health_float32.tflite")
with open(float_path, "wb") as f:
    f.write(tflite_float_model)
print(f"Float32 TFLite model saved: {len(tflite_float_model)} bytes -> {float_path}")


def evaluate_tflite(tflite_model_bytes, X, y, is_quantized=False, input_scale=None, input_zero_point=None, output_scale=None, output_zero_point=None):
    interpreter = tf.lite.Interpreter(model_content=tflite_model_bytes)
    interpreter.allocate_tensors()
    input_details = interpreter.get_input_details()[0]
    output_details = interpreter.get_output_details()[0]

    correct = 0
    tp = fp = tn = fn = 0
    for i in range(len(X)):
        sample = X[i:i+1].astype(np.float32)
        if is_quantized:
            sample = (sample / input_scale + input_zero_point).astype(input_details["dtype"])
        interpreter.set_tensor(input_details["index"], sample)
        interpreter.invoke()
        out = interpreter.get_tensor(output_details["index"])
        if is_quantized:
            out = (out.astype(np.float32) - output_zero_point) * output_scale
        pred = 1 if out[0][0] > 0.5 else 0
        actual = y[i]
        if pred == actual:
            correct += 1
        if pred == 1 and actual == 1:
            tp += 1
        elif pred == 1 and actual == 0:
            fp += 1
        elif pred == 0 and actual == 0:
            tn += 1
        else:
            fn += 1

    acc = correct / len(X)
    recall = tp / (tp + fn) if (tp + fn) > 0 else 0
    return acc, recall, (tp, fp, tn, fn)


acc, recall, cm = evaluate_tflite(tflite_float_model, X_test_scaled, y_test)
print(f"Float32 TFLite - accuracy: {acc:.3f}, degrading_recall: {recall:.3f}, (tp,fp,tn,fn)={cm}")

def representative_dataset():
    for i in range(len(X_train_scaled)):
        yield [X_train_scaled[i:i+1].astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_quant_model = converter.convert()

quant_path = os.path.join(DATA_DIR, "battery_health_int8.tflite")
with open(quant_path, "wb") as f:
    f.write(tflite_quant_model)
print(f"\nInt8 quantized TFLite model saved: {len(tflite_quant_model)} bytes -> {quant_path}")

interpreter = tf.lite.Interpreter(model_content=tflite_quant_model)
interpreter.allocate_tensors()
input_details = interpreter.get_input_details()[0]
output_details = interpreter.get_output_details()[0]

input_scale, input_zero_point = input_details["quantization"]
output_scale, output_zero_point = output_details["quantization"]

print(f"\nInput quantization: scale={input_scale}, zero_point={input_zero_point}")
print(f"Output quantization: scale={output_scale}, zero_point={output_zero_point}")

acc_q, recall_q, cm_q = evaluate_tflite(
    tflite_quant_model, X_test_scaled, y_test,
    is_quantized=True,
    input_scale=input_scale, input_zero_point=input_zero_point,
    output_scale=output_scale, output_zero_point=output_zero_point,
)
print(f"\nInt8 quantized TFLite - accuracy: {acc_q:.3f}, degrading_recall: {recall_q:.3f}, (tp,fp,tn,fn)={cm_q}")

print(f"\n--- Size comparison ---")
print(f"Original Keras model: {os.path.getsize(MODEL_PATH)} bytes")
print(f"Float32 TFLite: {len(tflite_float_model)} bytes")
print(f"Int8 quantized TFLite: {len(tflite_quant_model)} bytes")
print(f"Compression ratio (float32 -> int8): {len(tflite_float_model) / len(tflite_quant_model):.1f}x")

quant_params = {
    "input_scale": float(input_scale),
    "input_zero_point": int(input_zero_point),
    "output_scale": float(output_scale),
    "output_zero_point": int(output_zero_point),
}
with open(os.path.join(DATA_DIR, "quant_params.json"), "w") as f:
    json.dump(quant_params, f, indent=2)
print(f"\nQuantization params saved to quant_params.json")
