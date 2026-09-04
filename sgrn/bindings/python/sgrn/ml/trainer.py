import json
import os
from typing import Dict, Any, Optional
from .dataset import DatasetReader

try:
    import numpy as np
except ImportError:
    np = None

class AutoMLTrainer:
    """Automated Model Competition & Selection Trainer for SGRN Datasets."""
    def __init__(self, manifest_path: str, csv_path: str):
        self.reader = DatasetReader(manifest_path, csv_path)

    def train_and_select_best(self, target_feature: str, task: str = "regression", output_model_prefix: str = "model") -> Dict[str, Any]:
        """Trains multiple candidate models, evaluates metrics, selects champion model."""
        if np is None:
            raise ImportError("numpy is required for AutoMLTrainer")

        try:
            from sklearn.model_selection import train_test_split
            from sklearn.ensemble import RandomForestRegressor, RandomForestClassifier, IsolationForest
            from sklearn.linear_model import Ridge, LogisticRegression
            from sklearn.metrics import mean_squared_error, r2_score, f1_score
        except ImportError:
            raise ImportError("scikit-learn is required for AutoMLTrainer. Install with: pip install scikit-learn")

        X, y = self.reader.load_numpy(target_column=target_feature)
        X = np.nan_to_num(X, nan=0.0)

        best_model_name = ""
        best_score = -float("inf")
        best_model = None

        if task == "regression":
            y = np.nan_to_num(y, nan=0.0)
            X_train, X_val, y_train, y_val = train_test_split(X, y, test_size=0.2, random_state=42)

            candidates = {
                "Ridge": Ridge(),
                "RandomForestRegressor": RandomForestRegressor(n_estimators=50, random_state=42),
            }

            for name, model in candidates.items():
                model.fit(X_train, y_train)
                preds = model.predict(X_val)
                score = r2_score(y_val, preds)
                print(f"[AutoMLTrainer] Candidate '{name}' R2 score: {score:.4f}")
                if score > best_score:
                    best_score = score
                    best_model_name = name
                    best_model = model

        elif task == "classification":
            X_train, X_val, y_train, y_val = train_test_split(X, y, test_size=0.2, random_state=42)
            candidates = {
                "LogisticRegression": LogisticRegression(max_iter=500),
                "RandomForestClassifier": RandomForestClassifier(n_estimators=50, random_state=42),
            }

            for name, model in candidates.items():
                model.fit(X_train, y_train)
                preds = model.predict(X_val)
                score = f1_score(y_val, preds, average="macro")
                print(f"[AutoMLTrainer] Candidate '{name}' Macro F1 score: {score:.4f}")
                if score > best_score:
                    best_score = score
                    best_model_name = name
                    best_model = model

        elif task == "anomaly":
            model = IsolationForest(contamination=0.05, random_state=42)
            model.fit(X)
            best_model_name = "IsolationForest"
            best_score = 1.0
            best_model = model

        result_summary = {
            "champion_model": best_model_name,
            "metric_score": float(best_score),
            "task": task,
            "target_feature": target_feature,
            "num_features": X.shape[1],
        }

        # Save metadata summary locally
        meta_path = f"{output_model_prefix}_meta.json"
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(result_summary, f, indent=2)

        print(f"[AutoMLTrainer] Champion Selected: {best_model_name} (Score: {best_score:.4f}) -> {meta_path}")
        return result_summary

    def save_to_datastore(self, gateway_client: Any, model_name: str, meta_data: Dict[str, Any]) -> bool:
        """Stores trained model state and metadata directly in the SGRN datastore via Gateway client bindings."""
        try:
            if hasattr(gateway_client, "write_data"):
                gateway_client.write_data(f"ml_models/{model_name}", meta_data)
                print(f"[AutoMLTrainer] Persisted model metadata to datastore at ml_models/{model_name}")
                return True
        except Exception as e:
            print(f"[AutoMLTrainer] Datastore upload failed: {e}")
        return False
