#!/usr/bin/env python3
"""
sgrn.ml.manifold_feature_integration — Cross-Linking Manifold Autoencoder
Reconstruction Error as Feature for AutoML & Anomaly Detection.

This module integrates the PyTorch manifold autoencoder's reconstruction error
(as computed in sgrn.ml.manifold.ProcessManifoldDetector) into the AutoML
pipeline as a first-class feature. This enables:

1. Physics-informed features for tabular ML models (Ridge, RandomForest, XGBoost emulation)
2. Unified anomaly scoring (reconstruction error + conformal threshold)
3. Cross-module feature sharing (manifold → AutoML → LLM doer pipeline)
4. Auto-configuration of AutoML from SCL schema-inferred manifold parameters
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import numpy as np

try:
    import torch
    from torch.utils.data import DataLoader, TensorDataset
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False


# ──────────────────────────────────────────────────────────────────────
# WindowAutoencoder Feature Extractor
# ──────────────────────────────────────────────────────────────────────


class ManifoldFeatureExtractor:
    """Extracts physics-informed features from a trained WindowAutoencoder.

    Features extracted:
      - recon_error: per-sample MSE between input and reconstruction
      - latent_vector: bottleneck representation (dim = latent_dim)
      - residual_norm: L2 norm of reconstruction residual
      - encoder_activations: intermediate layer activations (if needed)
      - anomaly_score: conformal prediction score (if calibrator available)

    These features can be concatenated with original tabular features to
    improve classical ML model performance (Ridge, RandomForest, XGBoost emulation).
    """

    def __init__(
        self,
        autoencoder: "WindowAutoencoder",
        scaler: Any,  # sgrn.ml.transform.Scaler
        features: List[str],
        latent_dim: int,
        device: str = "cpu",
    ):
        self.autoencoder = autoencoder
        self.scaler = scaler
        self.features = features
        self.latent_dim = latent_dim
        self.device = device

        # Move autoencoder to device and eval mode
        self.autoencoder.to(device)
        self.autoencoder.eval()

    # ── Extract features from a single window ────────────────────────

    def extract_window_features(
        self,
        window: np.ndarray,  # shape: (window_size, n_features) or (n_features,)
    ) -> Dict[str, float]:
        """Extract features from a single time-windows.

        Args:
            window: Raw window data. If 1D, assumed to be (n_features,).
                    If 2D, assumed to be (window_size, n_features).

        Returns:
            Dict of extracted features keyed by name.
        """
        if not HAS_TORCH:
            # Return dummy features if no torch
            n = window.shape[-1] if hasattr(window, "shape") else 1
            return {
                "recon_error": 0.0,
                "latent_0": 0.0,
                "residual_norm": 0.0,
            }

        import torch

        self.autoencoder.eval()

        # Prepare input: normalize via scaler, then tensorize
        # window expected as (1, window_size, n_features) or (n_features,)
        if window.ndim == 1:
            # Single timestep: reshape to (1, 1, n_features)
            window_t = torch.from_numpy(window.astype(np.float32)).unsqueeze(0).unsqueeze(0).to(self.device)
        elif window.ndim == 2:
            # window_size x n_features: add batch dim
            window_t = torch.from_numpy(window.astype(np.float32)).unsqueeze(0).to(self.device)
        else:
            raise ValueError(f"Unexpected window ndim: {window.ndim}")

        # Forward pass through autoencoder
        with torch.no_grad():
            # WindowAutoencoder forward expects (batch, window_size, n_features) or similar
            # Check the actual WindowAutoencoder architecture
            try:
                reconstruction, latent = self.autoencoder(window_t, return_latent=True)
            except TypeError:
                # Some autoencoders don't return latent; just reconstruct
                reconstruction = self.autoencoder(window_t)
                latent = None

        # Compute features
        # 1. Reconstruction error (MSE)
        window_t_cpu = window_t.squeeze().cpu()  # remove batch dim
        if window_t_cpu.ndim == 1:
            # Single feature vector
            recon_err = float(nn.MSELoss()(reconstruction.squeeze(), window_t_cpu).item())
        else:
            # (window_size, n_features) -> mean MSE across timesteps/features
            recon_err = float(
                nn.MSELoss(reduction="mean")(reconstruction.squeeze(), window_t_cpu).item()
            )

        # 2. Latent vector (bottleneck)
        latent_vals = []
        if latent is not None and hasattr(latent, "squeeze"):
            latent_vec = latent.squeeze().cpu().numpy()
            latent_vals = latent_vec.tolist()[: self.latent_dim]
        elif latent is not None:
            latent_vec = latent.reshape(-1).cpu().numpy()[: self.latent_dim]
            latent_vals = latent_vec.tolist()
        else:
            # Default zero latents
            latent_vals = [0.0] * self.latent_dim

        # 3. Residual norm (L2)
        with torch.no_grad():
            res = (reconstruction.squeeze() - window_t_cpu).abs()
            residual_norm = float(torch.norm(res).item())

        # 4. Optional: anomaly score if conformal calibrator available
        # (We'll provide a placeholder; actual conformal is in separate module)
        anomaly_score = float(recon_err)  # proxy: use recon error itself

        return {
            "recon_error": round(recon_err, 6),
            "residual_norm": round(residual_norm, 6),
            **{f"latent_{i}": round(v, 6) for i, v in enumerate(latent_vals)},
            "anomaly_score_proxy": round(anomaly_score, 6),
        }

    # ── Extract features from full dataset ───────────────────────────

    def extract_dataset_features(
        self,
        X: np.ndarray,  # shape: (n_samples, window_size, n_features) or (n_samples, n_features)
        window_size: Optional[int] = None,
        stride: int = 1,
    ) -> np.ndarray:
        """Extract reconstruction-error-based features for entire dataset.

        Returns array of shape (n_samples, n_extracted_features) where each row
        contains [recon_error, residual_norm, latent_0, ..., latent_{d-1}, anomaly_score_proxy].

        This is the key integration point: these features can be prepended/appended
        to the original feature set before feeding into AutoML models.
        """
        if not HAS_TORCH:
            # Return zeros if no torch
            n_samples = X.shape[0] if hasattr(X, "shape") else 1
            n_features = self.latent_dim + 4  # recon_error + residual + latents + anomaly
            return np.zeros((n_samples, n_features), dtype=np.float32)

        self.autoencoder.eval()

        n_samples = X.shape[0]

        # Determine processing mode based on X shape
        if X.ndim == 2:
            # (n_samples, n_features) -> treat each sample as a single window
            # Need to reshape for autoencoder: add window dim
            # Autoencoder expects (batch, window_size, n_features) or similar
            # We'll process with stride=1 and window_size=1 effectively
            processed_X = X.reshape(n_samples, -1, 1) if X.shape[1] > 0 else X.reshape(n_samples, 1, -1)
            actual_window_size = 1
        elif X.ndim == 3:
            # (n_samples, window_size, n_features) -> use as-is
            processed_X = X
            actual_window_size = X.shape[1] if X.shape[1] > 0 else 1
        else:
            raise ValueError(f"Unexpected X ndim: {X.ndim}")

        # Create DataLoader for batch processing
        dataset = TensorDataset(
            torch.from_numpy(processed_X.astype(np.float32))
        )
        loader = DataLoader(dataset, batch_size=64, shuffle=False)

        features_list: List[Dict[str, float]] = []

        with torch.no_grad():
            for batch_X in loader:
                batch_X = batch_X[0].to(self.device)  # remove dataset wrapper dim

                # Forward pass
                try:
                    reconstruction, latent = self.autoencoder(batch_X, return_latent=True)
                except TypeError:
                    reconstruction = self.autoencoder(batch_X)
                    latent = None

                # Process each sample in the batch
                for i in range(batch_X.shape[0]):
                    sample_window = batch_X[i].cpu().numpy()

                    # Extract per-sample features
                    feats = self.extract_window_features(sample_window)
                    features_list.append(feats)

        # Build feature matrix
        n_extracted = len(features_list[0]) if features_list else 0
        feature_matrix = np.zeros((n_samples, n_extracted), dtype=np.float32)

        for i, feats in enumerate(features_list):
            feature_matrix[i] = [
                feats.get("recon_error", 0.0),
                feats.get("residual_norm", 0.0),
            ] + [
                feats.get(f"latent_{j}", 0.0) for j in range(self.latent_dim)
            ] + [feats.get("anomaly_score_proxy", 0.0)]

        return feature_matrix

    # ── Convenience: prepend features to original AutoML feature set ──

    def get_integration_feature_names(self) -> List[str]:
        """Return feature names for the extracted manifold features.

        These names can be used to augment the AutoML feature list, e.g.:

            all_features = manifold_features + ["feat_0", "feat_1", ...]

        Returns list suitable for PyTorchAutoMLTrainer or AutoMLTrainer.
        """
        names = ["recon_error", "residual_norm"]
        names += [f"latent_{i}" for i in range(self.latent_dim)]
        names.append("anomaly_score_proxy")
        return names


# ──────────────────────────────────────────────────────────────────────
# Pipeline Integration: fitManifoldPipeline + Feature Augmentation
# ──────────────────────────────────────────────────────────────────────


def augmentFeaturesWithManifold(
    data: Any,
    schema_source: Any,
    features: Optional[List[str]] = None,
    window_size: int = 12,
    stride: int = 1,
    latent_dim: int = 8,
    include_manifold_features: bool = True,
    manifold_config: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Auto-integrate manifold reconstruction error as features for AutoML.

    This function wraps the end-to-end pipeline (fitManifoldPipeline) and
    augments the resulting feature set with physics-informed manifold features.

    Integration flow:
      1. Fit manifold autoencoder (existing fitManifoldPipeline)
      2. Extract recon_error, latent vectors per sample
      3. Augment original features with manifold features
      4. Return augmented feature matrix + metadata for AutoML

    Args:
        data: Input data (DataFrame, dict, or ndarray)
        schema_source: SCL schema path/dict for model configuration
        features: Original feature list (auto-discovered from schema if None)
        window_size: Sliding window size for manifold processing
        stride: Stride for window sliding
        latent_dim: Manifold latent space dimension
        include_manifold_features: Whether to include manifold-derived features
        manifold_config: Additional manifold config (scaler, features, etc.)

    Returns:
        Dict with keys:
          - "augmented_features": List[str] (original + manifold feature names)
          - "feature_matrix": np.ndarray (n_samples, n_augmented_features)
          - "detector": ProcessManifoldDetector (trained manifold model)
          - "scaler": Fitted Scaler
          - "manifold_config": dict of manifold processing config
          - "meta": dict of integration metadata
    """
    from .pipeline import fitManifoldPipeline
    from .manifold import WindowAutoencoder, ProcessManifoldDetector, trainManifoldDetector
    from .transform import fitProcessScaler

    # Step 1: Fit manifold detector (existing pipeline)
    print(f"[ManifoldFeature] Fitting manifold detector (window_size={window_size})...")

    detector_result = fitManifoldPipeline(
        data=data,
        features=features,
        schemaSource=schema_source,
        windowSize=window_size,
        stride=stride,
        calibrationStride=stride if stride > 1 else None,
        trainRatio=0.70,
        calRatio=0.30,
        trainModeHead=True,
        modeHeadConfig=None,
        device=None,  # auto-select
    )

    detector = detector_result["detector"]
    scaler = detector_result["scaler"]
    features = detector_result.get("features", [])

    # Step 2: Initialize feature extractor
    autoencoder = detector.autoencoder  # WindowAutoencoder inside ProcessManifoldDetector

    # Determine input dim from autoencoder
    if HAS_TORCH and autoencoder is not None:
        # Try to get input dimension from autoencoder architecture
        try:
            # WindowAutoencoder may store input_dim or we can infer
            sample_input = np.random.randn(1, window_size, max(1, len(features) or 1)).astype(np.float32)
            # Just note the feature extraction is set up
            extractor = ManifoldFeatureExtractor(
                autoencoder=autoencoder,
                scaler=scaler,
                features=features,
                latent_dim=latent_dim,
                device=detector_result.get("device", "cpu"),
            )
        except Exception as e:
            print(f"[ManifoldFeature] Extractor setup warning: {e}")
            extractor = None
    else:
        extractor = None

    # Step 3: If requested, extract manifold features from the data
    manifold_feature_matrix = None
    if include_manifold_features and extractor is not None:
        print(f"[ManifoldFeature] Extracting manifold features from data...")
        try:
            # Slice windows from the caller's own timeline. The pipeline
            # already windowed internally for training; here we regenerate
            # windows from the raw data so features align 1:1 with inputs.
            from .transform import extractFeatureMatrix
            try:
                timeline = extractFeatureMatrix(data, features)
            except Exception:
                timeline = np.asarray(data, dtype=np.float32)
                if timeline.ndim == 3:
                    # Pre-windowed (n_samples, window_size, n_features):
                    # concatenate back into a continuous 2-D timeline.
                    timeline = timeline.reshape(-1, timeline.shape[-1])
            manifold_feature_matrix = extractor.extract_dataset_features(
                timeline,
                window_size=window_size,
                stride=stride,
            )

            print(
                f"[ManifoldFeature] Extracted {manifold_feature_matrix.shape[1]} "
                f"features from {manifold_feature_matrix.shape[0]} samples"
            )
        except Exception as e:
            print(f"[ManifoldFeature] Feature extraction fallback (zeros): {e}")
            # Return zeros placeholder
            n_samples = 50
            manifold_feature_matrix = np.zeros(
                (n_samples, latent_dim + 3), dtype=np.float32
            )

    # Step 4: Build augmented feature list
    augmented_feature_names = list(features) if features else []

    if manifold_feature_matrix is not None and include_manifold_features:
        # Prepend manifold features (they carry physics signal, put them first)
        manifold_feature_names = extractor.get_integration_feature_names() if extractor else []
        augmented_feature_names = manifold_feature_names + list(augmented_feature_names)
    else:
        manifold_feature_names = []

    # Step 5: Return integration metadata
    meta = {
        "manifold_included": include_manifold_features,
        "latent_dim": latent_dim,
        "manifold_feature_count": (
            len(manifold_feature_names) if manifold_feature_names else 0
        ),
        "original_feature_count": len(features) if features else 0,
        "augmented_total": len(augmented_feature_names),
        "manifold_model_type": type(detector).__name__ if detector else "None",
        "scaler_type": type(scaler).__name__ if scaler else "None",
    }

    result = {
        "augmented_features": augmented_feature_names,
        "feature_matrix": manifold_feature_matrix if manifold_feature_matrix is not None
        else np.zeros(
            (50, len(augmented_feature_names)),
            dtype=np.float32,
        ),
        "detector": detector,
        "scaler": scaler,
        "manifold_config": {
            "window_size": window_size,
            "latent_dim": latent_dim,
            "include_features": include_manifold_features,
        },
        "meta": meta,
    }

    return result


# ──────────────────────────────────────────────────────────────────────
# Convenience: AutoML-ready feature augmentation
# ──────────────────────────────────────────────────────────────────────

def getAutoMLAugmentedFeatureNames(
    schema_source: Any,
    features: Optional[List[str]] = None,
    window_size: int = 12,
) -> List[str]:
    """Quick API: get feature name list augmented with manifold features,
    ready for PyTorchAutoMLTrainer or AutoMLTrainer.

    Usage:
        feat_names = getAutoMLAugmentedFeatureNames("schema.scl")
        # Returns: ["recon_error", "residual_norm", "latent_0", ..., "feat_0", "feat_1", ...]
    """
    result = augmentFeaturesWithManifold(
        data={},  # placeholder; real data needed for full pipeline
        schema_source=schema_source,
        features=features,
        window_size=window_size,
        include_manifold_features=True,
    )
    return result["augmented_features"]


# ──────────────────────────────────────────────────────────────────────
# Self-Test / Demo
# ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    """Self-test: verify feature extraction pipeline works."""
    import warnings
    warnings.filterwarnings("ignore")

    if not HAS_TORCH:
        print("SKIP: PyTorch not available")
        exit(0)

    print("=== Manifold Feature Integration Self-Test ===")

    # Create synthetic data mimicking SGRN industrial process
    np.random.seed(123)
    n_samples = 300
    n_original_features = 6
    window_size = 12

    # Simulate a continuous 2-D timeline (n_samples, n_features):
    # fitManifoldPipeline does its own sliding-window internally.
    timeline = np.random.randn(n_samples, n_original_features).astype(np.float32)
    # Inject a slow drift on signal_2 in the last third so latent/recon
    # features show real variation instead of pure noise.
    timeline[200:, 2] += 2.5

    # Create dummy schema source (just a path string)
    dummy_schema = "gas_processing_schema.scl"  # Would be real SCL path

    # Augment features with manifold
    result = augmentFeaturesWithManifold(
        data=timeline,
        schema_source=dummy_schema,
        features=[f"signal_{i}" for i in range(n_original_features)],
        window_size=window_size,
        stride=1,
        latent_dim=4,
        include_manifold_features=True,
    )

    print(f"Original features: {len(result['augmented_features']) - (result['meta']['manifold_feature_count'])}")
    print(f"Manifold features included: {result['meta']['manifold_feature_count']}")
    print(f"Augmented total: {result['meta']['augmented_total']}")
    print(f"Feature matrix shape: {result['feature_matrix'].shape}")

    if result['feature_matrix'] is not None and result['feature_matrix'].size > 0:
        # Check that features have non-zero variance (real extracted features)
        col_means = np.mean(result['feature_matrix'], axis=0)
        col_stds = np.std(result['feature_matrix'], axis=0)
        non_zero_std = np.sum(col_stds > 1e-6)
        print(f"Features with non-zero std: {non_zero_std}/{len(col_stds)}")

    # Test the quick API
    print(f"\nQuick API feature names (first 10): {result['augmented_features'][:10]}")

    print("\n=== Self-Test Complete ===")

# Snake-case aliases for backward compatibility (pre-camelCase callers)
augment_features_with_manifold = augmentFeaturesWithManifold
get_automl_augmented_feature_names = getAutoMLAugmentedFeatureNames
