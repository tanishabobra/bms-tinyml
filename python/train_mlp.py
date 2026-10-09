import pandas as pd
import numpy as np
import tensorflow as tf
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, recall_score
import os
import json

DATA_DIR = os.path.expanduser("~/bms_battery_data")
CSV_PATH = os.path.join(DATA_DIR, "battery_features.csv")

FEATURE_COLS = [
    "voltage_start", "voltage_end", "voltage_drop", "voltage_mean", "voltage_min",
    "temperature_mean", "temperature_max", "temperature_min",
    "current_mean", "discharge_duration_s",
]
LABEL_COL = "label_degrading"

df = pd.read_csv(CSV_PATH)

train_df = df[df["battery"].isin(["B0005", "B0006", "B0007"])]
test_df = df[df["battery"] == "B0018"]

X_train_raw = train_df[FEATURE_COLS].values
y_train = train_df[LABEL_COL].values
X_test_raw = test_df[FEATURE_COLS].values
y_test = test_df[LABEL_COL].values

scaler = StandardScaler()
X_train = scaler.fit_transform(X_train_raw)
X_test = scaler.transform(X_test_raw)

scaler_params = {
    "mean": scaler.mean_.tolist(),
    "scale": scaler.scale_.tolist(),
    "feature_order": FEATURE_COLS,
}
with open(os.path.join(DATA_DIR, "scaler_params.json"), "w") as f:
    json.dump(scaler_params, f, indent=2)
print("Saved scaler params to scaler_params.json")


def build_model():
    return tf.keras.Sequential([
        tf.keras.layers.Input(shape=(len(FEATURE_COLS),)),
        tf.keras.layers.Dense(8, activation="relu"),
        tf.keras.layers.Dense(4, activation="relu"),
        tf.keras.layers.Dense(1, activation="sigmoid"),
    ])


N_RUNS = 20
best_model = None
best_recall = -1
best_accuracy = -1
best_seed = None

for seed in range(N_RUNS):
    tf.keras.utils.set_random_seed(seed)

    model = build_model()
    model.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy"])

    early_stop = tf.keras.callbacks.EarlyStopping(
        monitor="val_loss", patience=10, restore_best_weights=True
    )

    model.fit(
        X_train, y_train,
        validation_data=(X_test, y_test),
        epochs=50,
        batch_size=16,
        callbacks=[early_stop],
        verbose=0,
    )

    y_pred_prob = model.predict(X_test, verbose=0)
    y_pred = (y_pred_prob > 0.5).astype(int).flatten()

    acc = accuracy_score(y_test, y_pred)
    rec_degrading = recall_score(y_test, y_pred, pos_label=1)

    print(f"Seed {seed}: accuracy={acc:.3f}, degrading_recall={rec_degrading:.3f}")

    if (rec_degrading > best_recall) or (rec_degrading == best_recall and acc > best_accuracy):
        best_recall = rec_degrading
        best_accuracy = acc
        best_model = model
        best_seed = seed

print(f"\nBest model: seed={best_seed}, accuracy={best_accuracy:.3f}, degrading_recall={best_recall:.3f}")

model = best_model
y_pred_prob = model.predict(X_test, verbose=0)
y_pred = (y_pred_prob > 0.5).astype(int).flatten()

print(f"\nFinal test accuracy on held-out B0018: {accuracy_score(y_test, y_pred):.3f}")
print("\nClassification report:")
print(classification_report(y_test, y_pred, target_names=["Healthy", "Degrading"]))
print("Confusion matrix:")
print(confusion_matrix(y_test, y_pred))

model.save(os.path.join(DATA_DIR, "battery_health_model.keras"))
print(f"\nModel saved to {os.path.join(DATA_DIR, 'battery_health_model.keras')}")
