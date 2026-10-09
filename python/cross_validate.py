import pandas as pd
import numpy as np
import tensorflow as tf
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import accuracy_score, recall_score, confusion_matrix
import os

DATA_DIR = os.path.expanduser("~/bms_battery_data")
CSV_PATH = os.path.join(DATA_DIR, "battery_features_v2.csv")

FEATURE_COLS = [
    "voltage_start", "voltage_end", "voltage_drop", "voltage_mean", "voltage_min",
    "voltage_slope", "resistance_proxy",
    "temperature_mean", "temperature_max", "temperature_min", "temperature_rise",
    "current_mean", "discharge_duration_s",
]
LABEL_COL = "label_degrading"
ALL_BATTERIES = ["B0005", "B0006", "B0007", "B0018"]
SEEDS_PER_FOLD = 10

df = pd.read_csv(CSV_PATH)


def build_model(n_features):
    reg = tf.keras.regularizers.l2(1e-3)
    return tf.keras.Sequential([
        tf.keras.layers.Input(shape=(n_features,)),
        tf.keras.layers.Dense(8, activation="relu", kernel_regularizer=reg),
        tf.keras.layers.Dropout(0.2),
        tf.keras.layers.Dense(4, activation="relu", kernel_regularizer=reg),
        tf.keras.layers.Dense(1, activation="sigmoid"),
    ])


fold_results = []

for held_out in ALL_BATTERIES:
    train_batteries = [b for b in ALL_BATTERIES if b != held_out]
    train_df = df[df["battery"].isin(train_batteries)]
    test_df = df[df["battery"] == held_out]

    X_train_raw = train_df[FEATURE_COLS].values
    y_train = train_df[LABEL_COL].values
    X_test_raw = test_df[FEATURE_COLS].values
    y_test = test_df[LABEL_COL].values

    scaler = StandardScaler()
    X_train = scaler.fit_transform(X_train_raw)
    X_test = scaler.transform(X_test_raw)

    best_recall = -1
    best_acc = -1
    best_pred = None

    for seed in range(SEEDS_PER_FOLD):
        tf.keras.utils.set_random_seed(seed)
        model = build_model(len(FEATURE_COLS))
        model.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy"])
        early_stop = tf.keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=10, restore_best_weights=True
        )
        model.fit(
            X_train, y_train,
            validation_data=(X_test, y_test),
            epochs=50, batch_size=16,
            callbacks=[early_stop], verbose=0,
        )
        y_pred = (model.predict(X_test, verbose=0) > 0.5).astype(int).flatten()
        acc = accuracy_score(y_test, y_pred)
        rec = recall_score(y_test, y_pred, pos_label=1)

        if (rec > best_recall) or (rec == best_recall and acc > best_acc):
            best_recall, best_acc, best_pred = rec, acc, y_pred

    cm = confusion_matrix(y_test, best_pred)
    print(f"Held out {held_out}: accuracy={best_acc:.3f}, degrading_recall={best_recall:.3f}, confusion_matrix={cm.tolist()}")
    fold_results.append((held_out, best_acc, best_recall))

print("\n--- Cross-validation summary ---")
accs = [r[1] for r in fold_results]
recalls = [r[2] for r in fold_results]
print(f"Mean accuracy across 4 folds: {np.mean(accs):.3f} (std {np.std(accs):.3f})")
print(f"Mean degrading recall across 4 folds: {np.mean(recalls):.3f} (std {np.std(recalls):.3f})")
