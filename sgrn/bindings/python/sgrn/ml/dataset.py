import json
import gzip
import os
from typing import Dict, Any, Tuple, Optional, List

try:
    import numpy as np
except ImportError:
    np = None

try:
    import pandas as pd
except ImportError:
    pd = None

class DatasetReader:
    """Reads sgrn_dataset manifests and normalized CSV telemetry records into NumPy/Pandas."""
    def __init__(self, manifest_path: str, csv_path: str):
        self.manifest_path = manifest_path
        self.csv_path = csv_path
        self.manifest: Dict[str, Any] = {}
        self.features: List[Dict[str, Any]] = []
        self._load_manifest()

    def _load_manifest(self) -> None:
        if not os.path.exists(self.manifest_path):
            raise FileNotFoundError(f"Manifest not found: {self.manifest_path}")
        with open(self.manifest_path, "r", encoding="utf-8") as f:
            self.manifest = json.load(f)
        self.features = self.manifest.get("features", [])

    def get_feature_names(self, categorical_only: bool = False, continuous_only: bool = False) -> List[str]:
        names = []
        for feat in self.features:
            if categorical_only and not feat.get("is_categorical", False):
                continue
            if continuous_only and feat.get("is_categorical", False):
                continue
            names.append(feat["name"])
        return names

    def load_pandas(self) -> Optional["pd.DataFrame"]:
        if pd is None:
            raise ImportError("pandas library is required for load_pandas()")
        
        dtype_map = {}
        for feat in self.features:
            name = feat["name"]
            if feat.get("is_categorical", False):
                dtype_map[name] = "category"
            else:
                dtype_map[name] = "float32"

        df = pd.read_csv(self.csv_path, dtype=dtype_map)
        
        # Apply enum label mapping if present
        for feat in self.features:
            name = feat["name"]
            enum_vals = feat.get("enum_values")
            if enum_vals and name in df.columns:
                mapping = {int(k) if k.isdigit() else k: v for k, v in enum_vals.items()}
                df[name] = df[name].map(mapping).astype("category")

        # Forward fill and backward fill initial sensor NaNs
        df = df.ffill().bfill()
        num_cols = df.select_dtypes(include=["number", "float", "int"]).columns
        df[num_cols] = df[num_cols].fillna(0.0)
        return df

    def load_numpy(self, target_column: Optional[str] = None) -> Tuple[Any, Optional[Any]]:
        if np is None:
            raise ImportError("numpy library is required for load_numpy()")
        
        df = self.load_pandas()
        
        # Convert any categorical columns to integer codes for numpy compatibility
        for col in df.select_dtypes(include=["category"]).columns:
            df[col] = df[col].cat.codes

        if target_column:
            if target_column not in df.columns:
                raise KeyError(f"Target column '{target_column}' not in dataset")
            feature_cols = [c for c in df.columns if c != target_column]
            X = df[feature_cols].to_numpy(dtype=np.float32)
            y = df[target_column].to_numpy(dtype=np.float32)
            X = np.nan_to_num(X, nan=0.0)
            y = np.nan_to_num(y, nan=0.0)
            return X, y
        
        X = df.to_numpy(dtype=np.float32)
        X = np.nan_to_num(X, nan=0.0)
        return X, None
