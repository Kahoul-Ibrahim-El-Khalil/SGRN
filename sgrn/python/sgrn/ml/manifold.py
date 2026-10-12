#!/usr/bin/env python3
"""
sgrn.ml.manifold — Physical Process Manifold Autoencoder & Conformal Detector.

========================================================================================
THEORETICAL & MATHEMATICAL FOUNDATIONS:
========================================================================================

1. The Physical Manifold Hypothesis
-----------------------------------
Industrial continuous processes (e.g. hydrocarbon separation, distillation, amine treating,
refrigeration loops, flares) are governed by fundamental physical conservation and state laws:

  - Conservation of Mass:
      d/dt (m_cv) = sum(m_dot_in) - sum(m_dot_out)

  - Conservation of Energy (First Law of Thermodynamics):
      d/dt (U_cv) = Q_dot - W_dot + sum(m_dot_in * h_in) - sum(m_dot_out * h_out)

  - Real-Gas Thermodynamics (Equation of State, e.g., Peng-Robinson):
      P = (R * T) / (v - b) - a(T) / (v * (v + b) + b * (v - b))

  - Closed-Loop Controller Constraints (PID, Cascade, Anti-Surge):
      u(t) = K_p * e(t) + K_i * int(e(tau) dtau) + K_d * de/dt

Because of these algebraic and differential relationships, although the plant streams
produce high-dimensional telemetry x in R^D (e.g., D = 12 signals across W = 20 timesteps,
giving a 240-dimensional space), the actual physically attainable nominal operating states
lie on a much lower-dimensional smooth manifold:

      M subset R^D,   dim(M) = d << D

Here d corresponds to the true physical degrees of freedom (feed rate, ambient temperature,
cooling water supply temperature, separator pressure setpoint, etc.).


2. Autoencoder as a Non-Linear Projection Operator
--------------------------------------------------
A neural autoencoder consists of an encoder f_enc: R^D -> R^d and a decoder f_dec: R^d -> R^D,
passing through a low-dimensional bottleneck latent space z in R^d:

      z = f_enc(x; theta_enc)
      x_hat = f_dec(z; theta_dec) = Proj_M(x)

Trained with Mean Squared Error (MSE) strictly on nominal operational telemetry:
      L(theta) = E_{x in M} [ || x - f_dec(f_enc(x)) ||^2 ]

The composition f_dec o f_enc approximates the non-linear orthogonal projection operator
onto the nominal process manifold M.


3. Orthogonal Residual Distance as Anomaly Metric
-------------------------------------------------
For any arbitrary observation window x_tilde:
  - If the plant is operating normally, x_tilde in M (up to sensor noise epsilon ~ N(0, Sigma)).
    The reconstructed window x_hat matches x_tilde, yielding a near-zero reconstruction error.
  - If a fault occurs (e.g. hydrate plug, blocked flare, valve stiction, unmetered leakage),
    the physical conservation laws are broken. The point x_tilde departs from M along an
    orthogonal normal vector:

      dist^2(x_tilde, M) = || x_tilde - Proj_M(x_tilde) ||^2 = || r ||^2

This orthogonal distance serves as an invariant physical anomaly score e(x).


4. Split-Conformal Prediction & 3-State Uncertainty Classification
------------------------------------------------------------------
Rather than relying on arbitrary heuristics (e.g. 3*sigma), we calibrate the detection
threshold using split-conformal prediction on a held-out nominal calibration set D_cal:

Given non-conformity scores s_i = MSE(x_i, Proj_M(x_i)) for i in {1, ..., n_cal}, and a
user-specified significance level alpha (e.g., alpha = 0.05 for 95% nominal coverage):

      q_alpha = Quantile( (ceil((n_cal + 1)(1 - alpha)) / n_cal), {s_i} )

*Time-Series Dependence Note*:
Split-conformal coverage theorems assume exchangeability. Time-series windows exhibit
autocorrelation, which slightly weakens the exact non-exchangeable coverage bound. In practice,
the empirical calibration quantile on held-out post-training data provides a statistically
principled, robust threshold for false alarm regulation.

We structure operational output into a 3-State Uncertainty Framework:
  1. NORMAL:     score <= q_alpha
                 Observation lies within the nominal tolerance tube T_alpha around M.
  2. UNCERTAIN:  q_alpha < score <= k * q_alpha   (where k is typically 1.2 to 1.5)
                 Observation is in the boundary / transitional zone. The system advises
                 monitoring or soft alerts without triggering an immediate plant trip.
  3. ABNORMAL:   score > k * q_alpha
                 Statistically significant departure from M. Unambiguous physical fault.


5. Multi-Dimensional Error Decomposition (Closing the Loop)
-----------------------------------------------------------
The orthogonal residual vector r decomposes not just across raw signal indices, but
into high-level physical dimensions (Pressure, Temperature, Flow) and plant subsystems
(InletSeparation, SafetySystems) defined in the SCL schema:

      Delta E_dimension = sum_{j in dimension} e_j
      Delta E_subsystem = sum_{j in subsystem} e_j

Pinpointing the exact physical dimension and subsystem enables closed-loop automated
protective action: the Gateway writes model anomaly indicators and trip advice directly
back to S7 memory registers, completing the cyber-physical feedback loop.
========================================================================================
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

import numpy as np

try:
    import torch
    import torch.nn as nn
    from torch.utils.data import DataLoader, TensorDataset
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False
    nn = None

from .device import getHardwareInfo, resolveDevice
from .schema import (
    PHYSICAL_DIMENSION_MAP,
    configureModelFromSchema,
    inferDimensionFromName,
    inferPhysicalDimension,
    parseSclSchema,
)
from .transform import (
    Scaler,
    createSlidingWindows,
    extractFeatureMatrix,
    filterProcessTimeline,
    fitProcessScaler,
    splitProcessTimeline,
)


# ======================================================================================
# PyTorch Manifold Window Autoencoder Architecture
# ======================================================================================

if HAS_TORCH:
    class WindowAutoencoder(nn.Module):
        """Symmetric Multi-Layer Perceptron (MLP) Manifold Autoencoder."""

        def __init__(self, input_dim: int, hidden_dim: int = 64, latent_dim: int = 16):
            super().__init__()
            self.input_dim = input_dim
            self.hidden_dim = hidden_dim
            self.latent_dim = latent_dim

            self.encoder = nn.Sequential(
                nn.Linear(input_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, hidden_dim // 2),
                nn.ReLU(),
                nn.Linear(hidden_dim // 2, latent_dim),
            )

            self.decoder = nn.Sequential(
                nn.Linear(latent_dim, hidden_dim // 2),
                nn.ReLU(),
                nn.Linear(hidden_dim // 2, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, input_dim),
            )

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            latent = self.encoder(x)
            recon = self.decoder(latent)
            return recon
else:
    class WindowAutoencoder:  # type: ignore
        def __init__(self, *args, **kwargs):
            raise ImportError("PyTorch is required for WindowAutoencoder. Install torch to enable.")


# ======================================================================================
# Process Manifold Detector with Multi-Dimensional Deviation Attribution
# ======================================================================================

class ProcessManifoldDetector:
    """Production-grade anomaly detector combining Manifold Autoencoder with Split-Conformal Bounds.
    
    Supports multi-dimensional error decomposition:
      - Orthogonal residual breakdown across Physical Dimensions (Pressure, Temperature, Flow)
      - Breakdown across Plant Subsystems (InletSeparation, SafetySystems, etc.)
      - High-speed GPU batch evaluation on NVIDIA CUDA and AMD ROCm accelerators
    """

    def __init__(
        self,
        model: Any,
        scaler: Scaler,
        features: list[str],
        window_size: int,
        q_alpha: float,
        k_multiplier: float = 1.4,
        alpha: float = 0.05,
        device: Optional[Union[str, Any]] = None,
        feature_dimensions: Optional[Dict[str, str]] = None,
        feature_subsystems: Optional[Dict[str, str]] = None,
    ):
        self.scaler = scaler
        self.features = features
        self.window_size = window_size
        self.q_alpha = float(q_alpha)
        self.k_multiplier = float(k_multiplier)
        self.q_upper = float(q_alpha * k_multiplier)
        self.alpha = float(alpha)

        # Multi-dimensional taxonomy
        self.feature_dimensions = feature_dimensions or {
            f: inferDimensionFromName(f) for f in features
        }
        self.feature_subsystems = feature_subsystems or {
            f: f.split(".")[0] if "." in f else "General" for f in features
        }

        if HAS_TORCH and isinstance(model, nn.Module):
            self.device = resolveDevice(device)
            self.model = model.to(self.device)
            self.model.eval()
        else:
            self.model = model
            self.device = torch.device("cpu") if HAS_TORCH else None

    def to(self, device: Union[str, Any]) -> ProcessManifoldDetector:
        """Transfer neural network and evaluation operations to target device (e.g., 'cuda:0')."""
        if HAS_TORCH and isinstance(self.model, nn.Module):
            self.device = resolveDevice(device)
            self.model = self.model.to(self.device)
        return self

    @torch.no_grad() if HAS_TORCH else lambda f: f  # type: ignore
    def computeWindowError(
        self, windowNorm: np.ndarray
    ) -> Tuple[float, np.ndarray, np.ndarray]:
        """Compute overall window MSE and per-signal MSE across the temporal window."""
        flat_input = torch.from_numpy(windowNorm.reshape(1, -1)).float().to(self.device)
        flat_recon = self.model(flat_input)
        recon_norm = flat_recon.cpu().numpy().reshape(self.window_size, len(self.features))

        per_signal_mse = np.mean((windowNorm - recon_norm) ** 2, axis=0)
        total_score = float(np.mean(per_signal_mse))
        return total_score, per_signal_mse, recon_norm

    # Backward-compatible alias
    compute_window_error = computeWindowError

    def detect(self, rawWindow: np.ndarray) -> Dict[str, Any]:
        """Evaluate a raw engineering-unit telemetry window with multi-dimensional attribution."""
        raw_window = np.asarray(rawWindow, dtype=np.float32)
        if raw_window.shape != (self.window_size, len(self.features)):
            raise ValueError(
                f"Expected window shape ({self.window_size}, {len(self.features)}), got {raw_window.shape}"
            )

        norm_window = self.scaler.transform(raw_window)
        score, per_signal_mse, _ = self.computeWindowError(norm_window)

        # 3-State Conformal Uncertainty
        if score <= self.q_alpha:
            state = "normal"
        elif score <= self.q_upper:
            state = "uncertain"
        else:
            state = "abnormal"

        # Feature-Dimension Deviation Breakdown (each feature is a dimension d in R^D)
        total_err_sum = float(np.sum(per_signal_mse)) if np.sum(per_signal_mse) > 0 else 1.0

        # Error per feature dimension
        feature_dim_errs: Dict[str, float] = {
            self.features[idx]: float(mse_val) for idx, mse_val in enumerate(per_signal_mse)
        }

        # Subsystem error accumulation
        sub_errs: Dict[str, float] = {}
        for idx, mse_val in enumerate(per_signal_mse):
            feat = self.features[idx]
            sub = self.feature_subsystems.get(feat, "General")
            sub_errs[sub] = sub_errs.get(sub, 0.0) + float(mse_val)

        # Ranked breakdown across all feature dimensions
        dimension_breakdown = {
            feat: {
                "error": round(e, 4),
                "percentage": round(100.0 * e / total_err_sum, 2),
                "subsystem": self.feature_subsystems.get(feat, "General"),
                "unit": self.feature_dimensions.get(feat, "General"),
            }
            for feat, e in sorted(feature_dim_errs.items(), key=lambda x: x[1], reverse=True)
        }
        # The primary dimension is the specific feature that deviated the most
        primary_dim = max(feature_dim_errs.items(), key=lambda x: x[1])[0] if feature_dim_errs else "General"
        primary_sub = self.feature_subsystems.get(primary_dim, "General")

        subsystem_breakdown = {
            s: {
                "error_sum": round(e, 4),
                "percentage": round(100.0 * e / total_err_sum, 2),
            }
            for s, e in sorted(sub_errs.items(), key=lambda x: x[1], reverse=True)
        }

        # Top-3 Individual Signals with directional physical deviation
        top_idx = np.argsort(per_signal_mse)[::-1][:3]
        top_signals = []
        for idx in top_idx:
            feat_name = self.features[idx]
            current_val = float(raw_window[-1, idx])
            normal_mean = float(self.scaler.mean[idx])
            normal_std = float(self.scaler.std[idx])
            delta = current_val - normal_mean

            top_signals.append({
                "signal": feat_name,
                "dimension": feat_name,
                "subsystem": self.feature_subsystems.get(feat_name, "General"),
                "unit_dimension": self.feature_dimensions.get(feat_name, "General"),
                "reconstruction_error": float(per_signal_mse[idx]),
                "current_value": current_val,
                "normal_range": [normal_mean - 2.0 * normal_std, normal_mean + 2.0 * normal_std],
                "normal_mean": normal_mean,
                "deviation_delta": delta,
                "direction": f"{'+' if delta >= 0 else ''}{delta:.2f} relative to mean",
            })

        return {
            "score": score,
            "state": state,
            "primary_dimension": primary_dim,  # Top deviating feature dimension
            "primary_subsystem": primary_sub,  # Subsystem associated with primary dimension
            "dimension_breakdown": dimension_breakdown,  # Error per feature dimension
            "feature_deviations": feature_dim_errs,  # Raw MSE per feature dimension
            "subsystem_breakdown": subsystem_breakdown,
            "top_signals": top_signals,
            "q_alpha": self.q_alpha,
            "q_upper": self.q_upper,
        }

    @torch.no_grad() if HAS_TORCH else lambda f: f  # type: ignore
    def detectBatch(
        self,
        rawWindows: np.ndarray,
        batchSize: int = 256,
    ) -> Dict[str, Any]:
        """High-throughput vectorized batch detection accelerated on GPU/CPU."""
        raw_windows = np.asarray(rawWindows, dtype=np.float32)
        n_windows, w_size, n_feats = raw_windows.shape
        if w_size != self.window_size or n_feats != len(self.features):
            raise ValueError(
                f"Expected shape (*, {self.window_size}, {len(self.features)}), got {raw_windows.shape}"
            )

        norm_windows = (raw_windows - self.scaler.mean) / self.scaler.std
        input_dim = self.window_size * len(self.features)
        flat_all = norm_windows.reshape(n_windows, input_dim)

        scores = np.zeros(n_windows, dtype=np.float32)
        per_sig_all = np.zeros((n_windows, len(self.features)), dtype=np.float32)

        for start_idx in range(0, n_windows, batchSize):
            end_idx = min(n_windows, start_idx + batchSize)
            batch_flat = torch.from_numpy(flat_all[start_idx:end_idx]).float().to(self.device)
            batch_recon = self.model(batch_flat)

            b_size = end_idx - start_idx
            b_in = batch_flat.view(b_size, self.window_size, len(self.features))
            b_out = batch_recon.view(b_size, self.window_size, len(self.features))

            b_sig_mse = torch.mean((b_in - b_out) ** 2, dim=1)
            b_total_score = torch.mean(b_sig_mse, dim=1)

            scores[start_idx:end_idx] = b_total_score.cpu().numpy()
            per_sig_all[start_idx:end_idx] = b_sig_mse.cpu().numpy()

        states = []
        for s in scores:
            if s <= self.q_alpha:
                states.append("normal")
            elif s <= self.q_upper:
                states.append("uncertain")
            else:
                states.append("abnormal")

        primary_dimensions = []
        primary_subsystems = []
        for row in per_sig_all:
            top_feat_idx = int(np.argmax(row)) if len(row) > 0 else 0
            top_feat = self.features[top_feat_idx] if len(self.features) > top_feat_idx else "General"
            primary_dimensions.append(top_feat)
            primary_subsystems.append(self.feature_subsystems.get(top_feat, "General"))

        top_signals = []
        for row in per_sig_all:
            top_3 = [self.features[i] for i in np.argsort(row)[::-1][:3]]
            top_signals.append(top_3)

        return {
            "scores": scores,
            "states": states,
            "primary_dimensions": primary_dimensions,
            "primary_subsystems": primary_subsystems,
            "per_signal_mse": per_sig_all,
            "top_signals": top_signals,
            "q_alpha": self.q_alpha,
            "q_upper": self.q_upper,
        }

    # Backward-compatible alias
    detect_batch = detectBatch

    def save(self, saveDir: Union[str, Path]) -> None:
        """Save PyTorch weights and calibration metadata to disk."""
        save_dir = Path(saveDir)
        save_dir.mkdir(parents=True, exist_ok=True)
        torch.save(self.model.state_dict(), save_dir / "model.pt")

        meta = {
            "window_size": self.window_size,
            "input_dim": self.window_size * len(self.features),
            "features": self.features,
            "feature_dimensions": self.feature_dimensions,
            "feature_subsystems": self.feature_subsystems,
            "scaler": self.scaler.toDict(),
            "thresholds": {
                "alpha": self.alpha,
                "k_multiplier": self.k_multiplier,
                "q_alpha": self.q_alpha,
                "q_upper": self.q_upper,
            },
        }
        (save_dir / "model_meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    @classmethod
    def load(
        cls,
        saveDir: Union[str, Path],
        config: Optional[dict] = None,
        device: Optional[Union[str, Any]] = None,
    ) -> ProcessManifoldDetector:
        """Load trained model weights and conformal calibration metadata from directory."""
        save_dir = Path(saveDir)
        meta = json.loads((save_dir / "model_meta.json").read_text(encoding="utf-8"))
        scaler = Scaler.fromDict(meta["scaler"])
        features = meta["features"]
        feature_dimensions = meta.get("feature_dimensions")
        feature_subsystems = meta.get("feature_subsystems")
        window_size = meta["window_size"]
        input_dim = meta["input_dim"]

        hidden_dim = 64
        latent_dim = 16
        if config and "model" in config:
            hidden_dim = config["model"].get("hidden_dim", 64)
            latent_dim = config["model"].get("latent_dim", 16)

        target_device = resolveDevice(device)
        model = WindowAutoencoder(input_dim=input_dim, hidden_dim=hidden_dim, latent_dim=latent_dim)
        model.load_state_dict(torch.load(save_dir / "model.pt", map_location=target_device, weights_only=True))
        model.to(target_device)
        model.eval()

        th = meta["thresholds"]
        return cls(
            model=model,
            scaler=scaler,
            features=features,
            window_size=window_size,
            q_alpha=th["q_alpha"],
            k_multiplier=th["k_multiplier"],
            alpha=th["alpha"],
            device=target_device,
            feature_dimensions=feature_dimensions,
            feature_subsystems=feature_subsystems,
        )


# ======================================================================================
# GPU-Accelerated Model Training & Split-Conformal Calibration Loop
# ======================================================================================

def trainManifoldDetector(
    X_train: np.ndarray,
    X_cal: np.ndarray,
    scaler: Scaler,
    features: list[str],
    config: dict,
    seed: int = 42,
    device: Optional[Union[str, Any]] = None,
    featureDimensions: Optional[Dict[str, str]] = None,
    featureSubsystems: Optional[Dict[str, str]] = None,
) -> ProcessManifoldDetector:
    """Train manifold autoencoder on training split and compute conformal threshold on calibration split."""
    if not HAS_TORCH:
        raise ImportError("PyTorch is required to train manifold detectors.")

    torch.manual_seed(seed)
    np.random.seed(seed)

    target_device = resolveDevice(device)
    hw = getHardwareInfo()
    is_cuda = target_device.type == "cuda"
    print(f"[manifold_train] Platform: {hw['platform']} ({hw['backend']}) — Active: {target_device}")

    m_cfg = config.get("model", {})
    epochs = m_cfg.get("epochs", 50)
    batch_size = m_cfg.get("batch_size", 32)
    lr = m_cfg.get("learning_rate", 0.001)
    hidden_dim = m_cfg.get("hidden_dim", 64)
    latent_dim = m_cfg.get("latent_dim", 16)
    alpha = m_cfg.get("alpha", 0.05)
    k_mult = m_cfg.get("k_multiplier", 1.4)

    num_windows, window_size, num_features = X_train.shape
    input_dim = window_size * num_features

    X_train_flat = torch.from_numpy(X_train.reshape(num_windows, input_dim)).float()
    dataset = TensorDataset(X_train_flat, X_train_flat)
    loader = DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=True,
        pin_memory=is_cuda,
    )

    model = WindowAutoencoder(input_dim=input_dim, hidden_dim=hidden_dim, latent_dim=latent_dim)
    model.to(target_device)
    optimizer = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=1e-5)
    criterion = nn.MSELoss()

    model.train()
    for epoch in range(epochs):
        epoch_loss = 0.0
        for batch_x, _ in loader:
            batch_x = batch_x.to(target_device, non_blocking=is_cuda)
            optimizer.zero_grad()
            recon = model(batch_x)
            loss = criterion(recon, batch_x)
            loss.backward()
            optimizer.step()
            epoch_loss += loss.item() * len(batch_x)
        epoch_loss /= len(X_train_flat)
        if (epoch + 1) % 10 == 0 or epoch == epochs - 1:
            print(f"[manifold_train] Epoch {epoch + 1:2d}/{epochs:2d} — MSE Loss: {epoch_loss:.5f}")

    model.eval()

    # Split-Conformal Calibration on held-out X_cal
    n_cal = len(X_cal)
    X_cal_flat = torch.from_numpy(X_cal.reshape(n_cal, input_dim)).float()
    cal_scores = []

    with torch.no_grad():
        cal_batch_size = min(256, n_cal)
        for i in range(0, n_cal, cal_batch_size):
            b_cal = X_cal_flat[i : i + cal_batch_size].to(target_device, non_blocking=is_cuda)
            b_recon = model(b_cal)
            b_errs = torch.mean((b_cal - b_recon) ** 2, dim=1).cpu().numpy()
            cal_scores.extend(b_errs)

    cal_scores = np.array(cal_scores)
    quantile_level = min(1.0, np.ceil((n_cal + 1) * (1.0 - alpha)) / n_cal)
    q_alpha = float(np.quantile(cal_scores, quantile_level))

    print(f"[conformal] Calibration windows evaluated: {n_cal}")
    print(f"[conformal] q_{alpha:.2f} threshold (nominal {100*(1-alpha):.0f}% coverage): {q_alpha:.5f}")
    print(f"[conformal] Uncertain upper threshold ({k_mult}x multiplier): {q_alpha * k_mult:.5f}")

    return ProcessManifoldDetector(
        model=model,
        scaler=scaler,
        features=features,
        window_size=window_size,
        q_alpha=q_alpha,
        k_multiplier=k_mult,
        alpha=alpha,
        device=target_device,
        feature_dimensions=featureDimensions,
        feature_subsystems=featureSubsystems,
    )


# Backward-compatible snake_case alias
train_manifold_detector = trainManifoldDetector
