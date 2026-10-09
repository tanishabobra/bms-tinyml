import pandas as pd
from sklearn.tree import DecisionTreeClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix
import os

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

print(f"Training rows: {len(train_df)}")
print(f"Test rows (held-out B0018): {len(test_df)}")

X_train = train_df[FEATURE_COLS]
y_train = train_df[LABEL_COL]
X_test = test_df[FEATURE_COLS]
y_test = test_df[LABEL_COL]

clf = DecisionTreeClassifier(max_depth=4, random_state=42)
clf.fit(X_train, y_train)

y_pred = clf.predict(X_test)

print(f"\nTest accuracy on held-out B0018: {accuracy_score(y_test, y_pred):.3f}")
print("\nClassification report:")
print(classification_report(y_test, y_pred, target_names=["Healthy", "Degrading"]))
print("Confusion matrix:")
print(confusion_matrix(y_test, y_pred))

print("\nFeature importances:")
importances = sorted(zip(FEATURE_COLS, clf.feature_importances_), key=lambda x: -x[1])
for name, imp in importances:
    print(f"  {name}: {imp:.3f}")
