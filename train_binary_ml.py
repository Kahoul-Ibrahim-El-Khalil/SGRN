#!/usr/bin/env python3
import sys
import os
import argparse
import json
import numpy as np

# Ensure sgrn python bindings are in import path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'sgrn/bindings/python'))

try:
    from sgrn.ml.dataset import BinaryDatasetReader
except ImportError as e:
    print(f"Error: Could not import BinaryDatasetReader from sgrn.ml.dataset: {e}")
    sys.exit(1)

try:
    from sklearn.model_selection import train_test_split
    from sklearn.ensemble import RandomForestRegressor, RandomForestClassifier, IsolationForest
    from sklearn.linear_model import Ridge, LogisticRegression
    from sklearn.metrics import r2_score, mean_squared_error, accuracy_score, f1_score
except ImportError:
    print("Error: scikit-learn is required. Install with: pip install scikit-learn")
    sys.exit(1)

try:
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False

def run_interactive_pipeline(binary_path: str):
    print("==================================================")
    print(" SGRN High-Density Binary (.bin.zst) ML Pipeline")
    print("==================================================")
    print(f"Loading archives from: {binary_path} ...")

    reader = BinaryDatasetReader(binary_path)
    timestamps, feature_names, X = reader.read_dataset()

    if X.shape[0] == 0:
        print("Error: No telemetry records extracted from binary archives.")
        sys.exit(1)

    print(f"\nSuccessfully parsed {X.shape[0]} records across {len(feature_names)} features!")
    print(f"Time span: {timestamps[0]} ms -> {timestamps[-1]} ms (Duration: {(timestamps[-1] - timestamps[0])/1000.0:.2f} s)")

    print("\nAvailable Features:")
    for idx, name in enumerate(feature_names):
        print(f" [{idx:2d}] {name}")

    # Prompt for Target Feature selection
    print("\nSelect target feature to predict:")
    while True:
        try:
            choice = input(f"Enter target index (0-{len(feature_names)-1}) [default: 0]: ").strip()
            target_idx = 0 if choice == "" else int(choice)
            if 0 <= target_idx < len(feature_names):
                break
            print("Invalid index. Try again.")
        except ValueError:
            print("Please enter a valid integer.")

    target_name = feature_names[target_idx]
    print(f"\n-> Target Feature: {target_name}")

    # Prompt for Feature selection
    print("\nSelect input features (predictor columns):")
    print(" 1. All other features (Recommended)")
    print(" 2. Select specific indices")
    feat_choice = input("Select option [1]: ").strip()

    if feat_choice == "2":
        indices_str = input("Enter comma-separated feature indices (e.g. 1, 2, 4): ").strip()
        selected_indices = [int(i.strip()) for i in indices_str.split(",") if i.strip().isdigit()]
        if not selected_indices:
            selected_indices = [i for i in range(len(feature_names)) if i != target_idx]
    else:
        selected_indices = [i for i in range(len(feature_names)) if i != target_idx]

    input_feature_names = [feature_names[i] for i in selected_indices]
    print(f"-> Selected {len(input_feature_names)} input features: {input_feature_names}")

    # Extract X_matrix and y_vec
    X_input = X[:, selected_indices]
    y_target = X[:, target_idx]

    # Task selection
    print("\nSelect ML Task type:")
    print(" 1. Regression (Predict continuous sensor value)")
    print(" 2. Classification (Predict threshold / state category)")
    print(" 3. Anomaly Detection (Isolation Forest)")
    task_choice = input("Select task [1]: ").strip()

    task_type = "regression"
    if task_choice == "2":
        task_type = "classification"
    elif task_choice == "3":
        task_type = "anomaly"

    print(f"\n==================================================")
    print(f" Training Models for Task: {task_type.upper()}")
    print(f"==================================================")

    best_model = None
    best_name = ""
    best_score = -float('inf')
    y_pred_eval = None
    y_val_eval = None
    time_val_eval = None

    if task_type == "regression":
        X_train, X_val, y_train, y_val, t_train, t_val = train_test_split(
            X_input, y_target, timestamps, test_size=0.2, random_state=42, shuffle=False
        )

        models = {
            "Ridge Regression": Ridge(),
            "Random Forest Regressor": RandomForestRegressor(n_estimators=50, random_state=42)
        }

        for name, model in models.items():
            model.fit(X_train, y_train)
            preds = model.predict(X_val)
            r2 = r2_score(y_val, preds)
            rmse = np.sqrt(mean_squared_error(y_val, preds))
            print(f" [{name}] R2 Score: {r2:.4f} | RMSE: {rmse:.4f}")
            if r2 > best_score:
                best_score = r2
                best_name = name
                best_model = model
                y_pred_eval = preds
                y_val_eval = y_val
                time_val_eval = t_val

    elif task_type == "classification":
        # Convert target into binary high/low state for classification demo
        median_val = np.median(y_target)
        y_class = (y_target > median_val).astype(int)
        
        X_train, X_val, y_train, y_val, t_train, t_val = train_test_split(
            X_input, y_class, timestamps, test_size=0.2, random_state=42, shuffle=False
        )

        models = {
            "Logistic Regression": LogisticRegression(max_iter=500),
            "Random Forest Classifier": RandomForestClassifier(n_estimators=50, random_state=42)
        }

        for name, model in models.items():
            model.fit(X_train, y_train)
            preds = model.predict(X_val)
            acc = accuracy_score(y_val, preds)
            f1 = f1_score(y_val, preds, average='macro')
            print(f" [{name}] Accuracy: {acc:.4f} | Macro F1: {f1:.4f}")
            if f1 > best_score:
                best_score = f1
                best_name = name
                best_model = model
                y_pred_eval = preds
                y_val_eval = y_val
                time_val_eval = t_val

    elif task_type == "anomaly":
        model = IsolationForest(contamination=0.05, random_state=42)
        model.fit(X_input)
        preds = model.predict(X_input) # -1 for anomaly, 1 for normal
        best_name = "IsolationForest"
        best_score = 1.0
        best_model = model
        y_pred_eval = preds
        y_val_eval = y_target
        time_val_eval = timestamps

    print(f"\n==================================================")
    print(f" Champion Selected: {best_name} (Metric Score: {best_score:.4f})")
    print(f"==================================================")

    # Visualization
    if HAS_MATPLOTLIB and y_pred_eval is not None:
        print("\nOpening interactive data visualizer window...")
        plt.figure(figsize=(12, 6))
        
        # Convert relative timestamps in seconds
        t_sec = (time_val_eval - timestamps[0]) / 1000.0
        
        if task_type == "regression":
            plt.plot(t_sec, y_val_eval, label=f"Actual '{target_name}'", color="navy", alpha=0.8, linewidth=1.5)
            plt.plot(t_sec, y_pred_eval, label=f"Predicted by {best_name}", color="crimson", linestyle="--", linewidth=1.5)
            plt.title(f"Direct Binary WAL Model Prediction: {target_name} ({best_name})")
            plt.ylabel("Value")

        elif task_type == "classification":
            plt.plot(t_sec, y_val_eval, label="Actual Class", color="navy", alpha=0.7)
            plt.step(t_sec, y_pred_eval, label=f"Predicted Class ({best_name})", color="crimson", where='post', linestyle="--")
            plt.title(f"Direct Binary WAL Classification: {target_name}")
            plt.ylabel("State Class (0/1)")

        elif task_type == "anomaly":
            plt.plot(t_sec, y_val_eval, label=f"Signal '{target_name}'", color="teal")
            anomaly_idx = np.where(y_pred_eval == -1)[0]
            plt.scatter(t_sec[anomaly_idx], y_val_eval[anomaly_idx], color="red", label="Detected Anomaly", zorder=5)
            plt.title(f"Direct Binary WAL Anomaly Detection ({best_name})")
            plt.ylabel("Value")

        plt.xlabel("Time (seconds)")
        plt.grid(True, linestyle=":", alpha=0.6)
        plt.legend(loc="upper right")
        plt.tight_layout()
        plt.show()
    else:
        if not HAS_MATPLOTLIB:
            print("[Visualizer] matplotlib not installed. Install with: pip install matplotlib")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Interactive Direct Binary (.bin.zst) ML Trainer & Visualizer")
    parser.add_argument("path", help="Path to .bin.zst file or directory containing binary WAL archives")
    parser.add_argument("-s", "--scl", help="Optional path to SCL schema file if binary header schema is empty", default=None)
    args = parser.parse_args()

    def run_interactive_pipeline_with_scl(binary_path: str, scl_path: Optional[str] = None):
        print("==================================================")
        print(" SGRN High-Density Binary (.bin.zst) ML Pipeline")
        print("==================================================")
        print(f"Loading archives from: {binary_path} ...")

        reader = BinaryDatasetReader(binary_path, scl_schema_path=scl_path)
        timestamps, feature_names, X = reader.read_dataset()

        if X.shape[0] == 0:
            print("Error: No telemetry records extracted from binary archives.")
            sys.exit(1)

        print(f"\nSuccessfully parsed {X.shape[0]} records across {len(feature_names)} features!")
        if len(timestamps) > 0:
            print(f"Time span: {timestamps[0]} ms -> {timestamps[-1]} ms (Duration: {(timestamps[-1] - timestamps[0])/1000.0:.2f} s)")

        print("\nAvailable Features:")
        for idx, name in enumerate(feature_names):
            print(f" [{idx:2d}] {name}")

        print("\nSelect target feature to predict:")
        while True:
            try:
                choice = input(f"Enter target index (0-{len(feature_names)-1}) [default: 0]: ").strip()
                target_idx = 0 if choice == "" else int(choice)
                if 0 <= target_idx < len(feature_names):
                    break
                print("Invalid index. Try again.")
            except ValueError:
                print("Please enter a valid integer.")

        target_name = feature_names[target_idx]
        print(f"\n-> Target Feature: {target_name}")

        print("\nSelect input features (predictor columns):")
        print(" 1. All other features (Recommended)")
        print(" 2. Select specific indices")
        feat_choice = input("Select option [1]: ").strip()

        if feat_choice == "2":
            indices_str = input("Enter comma-separated feature indices (e.g. 1, 2, 4): ").strip()
            selected_indices = [int(i.strip()) for i in indices_str.split(",") if i.strip().isdigit()]
            if not selected_indices:
                selected_indices = [i for i in range(len(feature_names)) if i != target_idx]
        else:
            selected_indices = [i for i in range(len(feature_names)) if i != target_idx]

        input_feature_names = [feature_names[i] for i in selected_indices]
        print(f"-> Selected {len(input_feature_names)} input features: {input_feature_names}")

        X_input = X[:, selected_indices]
        y_target = X[:, target_idx]

        print("\nSelect ML Task type:")
        print(" 1. Regression (Predict continuous sensor value)")
        print(" 2. Classification (Predict threshold / state category)")
        print(" 3. Anomaly Detection (Isolation Forest)")
        task_choice = input("Select task [1]: ").strip()

        task_type = "regression"
        if task_choice == "2":
            task_type = "classification"
        elif task_choice == "3":
            task_type = "anomaly"

        print(f"\n==================================================")
        print(f" Training Models for Task: {task_type.upper()}")
        print(f"==================================================")

        best_model = None
        best_name = ""
        best_score = -float('inf')
        y_pred_eval = None
        y_val_eval = None
        time_val_eval = None

        if task_type == "regression":
            X_train, X_val, y_train, y_val, t_train, t_val = train_test_split(
                X_input, y_target, timestamps, test_size=0.2, random_state=42, shuffle=False
            )

            models = {
                "Ridge Regression": Ridge(),
                "Random Forest Regressor": RandomForestRegressor(n_estimators=50, random_state=42)
            }

            for name, model in models.items():
                model.fit(X_train, y_train)
                preds = model.predict(X_val)
                r2 = r2_score(y_val, preds)
                rmse = np.sqrt(mean_squared_error(y_val, preds))
                print(f" [{name}] R2 Score: {r2:.4f} | RMSE: {rmse:.4f}")
                if r2 > best_score:
                    best_score = r2
                    best_name = name
                    best_model = model
                    y_pred_eval = preds
                    y_val_eval = y_val
                    time_val_eval = t_val

        elif task_type == "classification":
            median_val = np.median(y_target)
            y_class = (y_target > median_val).astype(int)
            
            X_train, X_val, y_train, y_val, t_train, t_val = train_test_split(
                X_input, y_class, timestamps, test_size=0.2, random_state=42, shuffle=False
            )

            models = {
                "Logistic Regression": LogisticRegression(max_iter=500),
                "Random Forest Classifier": RandomForestClassifier(n_estimators=50, random_state=42)
            }

            for name, model in models.items():
                model.fit(X_train, y_train)
                preds = model.predict(X_val)
                acc = accuracy_score(y_val, preds)
                f1 = f1_score(y_val, preds, average='macro')
                print(f" [{name}] Accuracy: {acc:.4f} | Macro F1: {f1:.4f}")
                if f1 > best_score:
                    best_score = f1
                    best_name = name
                    best_model = model
                    y_pred_eval = preds
                    y_val_eval = y_val
                    time_val_eval = t_val

        elif task_type == "anomaly":
            model = IsolationForest(contamination=0.05, random_state=42)
            model.fit(X_input)
            preds = model.predict(X_input)
            best_name = "IsolationForest"
            best_score = 1.0
            best_model = model
            y_pred_eval = preds
            y_val_eval = y_target
            time_val_eval = timestamps

        print(f"\n==================================================")
        print(f" Champion Selected: {best_name} (Metric Score: {best_score:.4f})")
        print(f"==================================================")

        if HAS_MATPLOTLIB and y_pred_eval is not None:
            print("\nOpening interactive data visualizer window...")
            plt.figure(figsize=(12, 6))
            t_sec = (time_val_eval - timestamps[0]) / 1000.0 if len(timestamps) > 0 else np.arange(len(y_val_eval))
            
            if task_type == "regression":
                plt.plot(t_sec, y_val_eval, label=f"Actual '{target_name}'", color="navy", alpha=0.8, linewidth=1.5)
                plt.plot(t_sec, y_pred_eval, label=f"Predicted by {best_name}", color="crimson", linestyle="--", linewidth=1.5)
                plt.title(f"Direct Binary WAL Model Prediction: {target_name} ({best_name})")
                plt.ylabel("Value")

            elif task_type == "classification":
                plt.plot(t_sec, y_val_eval, label="Actual Class", color="navy", alpha=0.7)
                plt.step(t_sec, y_pred_eval, label=f"Predicted Class ({best_name})", color="crimson", where='post', linestyle="--")
                plt.title(f"Direct Binary WAL Classification: {target_name}")
                plt.ylabel("State Class (0/1)")

            elif task_type == "anomaly":
                plt.plot(t_sec, y_val_eval, label=f"Signal '{target_name}'", color="teal")
                anomaly_idx = np.where(y_pred_eval == -1)[0]
                plt.scatter(t_sec[anomaly_idx], y_val_eval[anomaly_idx], color="red", label="Detected Anomaly", zorder=5)
                plt.title(f"Direct Binary WAL Anomaly Detection ({best_name})")
                plt.ylabel("Value")

            plt.xlabel("Time (seconds)")
            plt.grid(True, linestyle=":", alpha=0.6)
            plt.legend(loc="upper right")
            plt.tight_layout()
            plt.show()

    run_interactive_pipeline_with_scl(args.path, args.scl)
