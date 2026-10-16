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
Split-conformal coverage theorems assume exchangeability. Sliding windows
with stride 1 overlap in W-1 timesteps, so consecutive scores are strongly
autocorrelated and the vanilla guarantee does not hold. This module
calibrates on non-overlapping windows (every ``calibrationStride``-th
window, defaulting to one window length — see ``trainManifoldDetector``):
block-level scores are far closer to exchangeable, restoring an
approximate block-coverage guarantee. Residual autocorrelation across
adjacent blocks means the nominal level remains approximate, not exact,
for time series — the threshold is statistically principled but not a
formal finite-sample guarantee under dependence.

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
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False
    pd = None

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
    applyStratifiedStats,
    contextLayout,
    contextRowKeys,
    createSlidingWindows,
    encodeContextMatrix,
    extractFeatureMatrix,
    extractRawContextColumns,
    filterProcessTimeline,
    fitProcessScaler,
    splitProcessTimeline,
)


# ======================================================================================
# PyTorch Manifold Window Autoencoder Architecture
# ======================================================================================

if HAS_TORCH:
    class WindowAutoencoder(nn.Module):
        """Symmetric Multi-Layer Perceptron (MLP) Manifold Autoencoder.

        Discrete operating context (one-hot Bool/#ENUM codes, flattened) is
        concatenated to the continuous window at the encoder input and
        conditions the bottleneck — the decoder reconstructs continuous
        signals only, so code distance never pollutes the anomaly score.
        ``context_dim=0`` reproduces the legacy unconditional architecture.
        """

        def __init__(self, input_dim: int, hidden_dim: int = 64, latent_dim: int = 16,
                     context_dim: int = 0, cont_dim: Optional[int] = None):
            super().__init__()
            self.input_dim = input_dim
            self.context_dim = int(context_dim or 0)
            self.cont_dim = int(cont_dim) if cont_dim else input_dim - self.context_dim
            self.hidden_dim = hidden_dim
            self.latent_dim = latent_dim

            enc_in = self.cont_dim + self.context_dim
            self.encoder = nn.Sequential(
                nn.Linear(enc_in, hidden_dim),
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
                nn.Linear(hidden_dim, self.cont_dim),
            )

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            latent = self.encoder(x)
            recon = self.decoder(latent)
            return recon

    class ModePredictor(nn.Module):
        """Predicts discrete operating mode FROM continuous physics only.

        The autoencoder answers "is this telemetry nominal?"; this head
        answers "which mode does the physics imply?". Its input is the
        continuous window alone — feeding it the actual mode codes would be
        a trivial copy shortcut, so a mismatch between predicted and
        actual mode (stuck valve, forced input, swapped sensor) surfaces as
        cross-entropy / binary-cross-entropy nonconformity instead.
        Outputs per-field logits in ``context_layout`` order: 1 logit per
        Bool (BCE), K logits per K-category enum (CE, unknown column
        excluded — an unseen category is handled by the unknown flag, not
        predicted).
        """

        def __init__(self, cont_dim: int, layout: List[Dict[str, Any]],
                     hidden_dim: int = 32):
            super().__init__()
            self.cont_dim = int(cont_dim)
            self.hidden_dim = int(hidden_dim)
            self.trunk = nn.Sequential(
                nn.Linear(cont_dim, hidden_dim), nn.ReLU(),
            )
            self.heads = nn.ModuleDict()
            self.layout_keys = []
            for entry in layout:
                key = f"h{len(self.heads)}"
                width = 1 if entry["kind"] == "bool" else max(1, entry["width"] - 1)
                self.heads[key] = nn.Sequential(
                    nn.Linear(hidden_dim, hidden_dim // 2), nn.ReLU(),
                    nn.Linear(hidden_dim // 2, width),
                )
                self.layout_keys.append(entry["feature"])

        def forward(self, x: torch.Tensor) -> Dict[str, torch.Tensor]:
            shared = self.trunk(x)
            return {k: head(shared) for k, head in self.heads.items()}
else:
    class WindowAutoencoder:  # type: ignore
        def __init__(self, *args, **kwargs):
            raise ImportError("PyTorch is required for WindowAutoencoder. Install torch to enable.")

    class ModePredictor:  # type: ignore
        def __init__(self, *args, **kwargs):
            raise ImportError("PyTorch is required for ModePredictor. Install torch to enable.")


# ======================================================================================
# Process Manifold Detector with Multi-Dimensional Deviation Attribution
# ======================================================================================

def _modeKeyFromEncoded(
    enc_last_row: np.ndarray,
    layout: List[Dict[str, Any]],
    specs: Optional[Dict[str, Any]] = None,
) -> List[str]:
    """Nominal-mode key: winning category per context field at the window's last step.

    Uses human-readable category names (``true``/``RUNNING``) when specs are
    given, else ``1``/``cat<i>`` fallbacks. Unknown categories -> ``unknown``.
    """
    specs = specs or {}
    key: List[str] = []
    for entry in layout:
        seg = enc_last_row[entry["offset"] : entry["offset"] + entry["width"]]
        cats = [str(c) for c in specs.get(entry["feature"], {}).get("categories", [])]
        if entry["kind"] == "bool":
            key.append("true" if float(seg[0]) >= 0.5 else "false")
        else:
            idx = int(np.argmax(seg)) if len(seg) else -1
            if idx < len(cats):
                key.append(cats[idx])
            else:
                key.append("unknown")
    return key


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
        q_alpha_per_signal: Optional[Union[List[float], np.ndarray]] = None,
        calibration_stride: Optional[int] = None,
        context_features: Optional[List[str]] = None,
        context_specs: Optional[Dict[str, Any]] = None,
        seen_modes: Optional[List[List[str]]] = None,
        stratify_fields: Optional[List[str]] = None,
        strata_stats: Optional[Dict[str, Any]] = None,
        mode_model: Any = None,
        q_mode_per_field: Optional[Dict[str, float]] = None,
    ):
        self.scaler = scaler
        self.features = features
        self.window_size = window_size
        self.q_alpha = float(q_alpha)
        self.k_multiplier = float(k_multiplier)
        self.q_upper = float(q_alpha * k_multiplier)
        self.alpha = float(alpha)
        self.calibration_stride = calibration_stride

        # Per-signal conformal thresholds (one q per feature). A single-sensor
        # fault (e.g. blocked flare) contributes only 1/F to the window-mean
        # MSE and is diluted below a global threshold; per-signal bounds flag
        # it directly. ``None`` (legacy bundles) disables per-signal states.
        if q_alpha_per_signal is not None:
            arr = np.asarray(q_alpha_per_signal, dtype=np.float64)
            if arr.shape != (len(features),):
                raise ValueError(
                    f"q_alpha_per_signal shape {arr.shape} does not match "
                    f"feature count {len(features)}"
                )
            self.q_alpha_per_signal = arr
            self.q_upper_per_signal = arr * float(k_multiplier)
        else:
            self.q_alpha_per_signal = None
            self.q_upper_per_signal = None

        # Discrete operating context (Bool flags + #ENUM modes). The codes
        # condition the encoder; they are never reconstructed, so a
        # STOPPED=0/RUNNING=2 gap contributes no Euclidean distance.
        self.context_features: List[str] = list(context_features or [])
        self.context_specs: Dict[str, Any] = dict(context_specs or {})
        self.context_layout = contextLayout(self.context_features, self.context_specs)
        self.context_dim = (
            sum(e["width"] for e in self.context_layout) if self.context_layout else 0
        )
        # Nominal mode keys (last-step category per context field) observed
        # during training. A window whose mode was never seen nominally is
        # escalated to at least ``uncertain`` — new operating regime, not
        # proof of fault. ``None`` (legacy bundles) disables the check.
        self.seen_modes = (
            {tuple(m) for m in seen_modes} if seen_modes is not None else None
        )
        # Mode-stratified standardisation (per-stratum mean/std; empty disables).
        self.stratify_fields: List[str] = list(stratify_fields or [])
        self.strata_stats: Dict[str, Any] = dict(strata_stats or {})
        # Physics->mode head: predicts discrete modes from continuous
        # windows alone. ``q_mode_per_field`` holds conformal NLL bounds.
        self.q_mode_per_field: Dict[str, float] = dict(q_mode_per_field or {})

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
        if HAS_TORCH and isinstance(mode_model, nn.Module):
            self.mode_model = mode_model.to(self.device)
            self.mode_model.eval()
        else:
            self.mode_model = mode_model

    def to(self, device: Union[str, Any]) -> ProcessManifoldDetector:
        """Transfer neural network and evaluation operations to target device (e.g., 'cuda:0')."""
        if HAS_TORCH and isinstance(self.model, nn.Module):
            self.device = resolveDevice(device)
            self.model = self.model.to(self.device)
        if HAS_TORCH and isinstance(getattr(self, "mode_model", None), nn.Module):
            self.mode_model = self.mode_model.to(self.device)
        return self

    @torch.no_grad() if HAS_TORCH else lambda f: f  # type: ignore
    def computeWindowError(
        self, windowNorm: np.ndarray, contextNorm: Optional[np.ndarray] = None
    ) -> Tuple[float, np.ndarray, np.ndarray]:
        """Compute overall window MSE and per-signal MSE across the temporal window.

        ``contextNorm`` is the flattened one-hot context window
        (``window_size * context_dim``); required exactly when the detector
        was configured with discrete context.
        """
        cont_flat = torch.from_numpy(windowNorm.reshape(1, -1)).float()
        if self.context_dim > 0:
            if contextNorm is None:
                raise ValueError(
                    "Detector expects discrete context "
                    f"({len(self.context_features)} signals); pass contextNorm "
                    "or the raw context to detect()."
                )
            ctx_flat = torch.from_numpy(
                np.asarray(contextNorm, dtype=np.float32).reshape(1, -1)
            ).float()
            flat_input = torch.cat([cont_flat, ctx_flat], dim=1).to(self.device)
        else:
            if contextNorm is not None:
                raise ValueError(
                    "Detector was trained without discrete context; "
                    "contextNorm is unexpected."
                )
            flat_input = cont_flat.to(self.device)
        flat_recon = self.model(flat_input)
        recon_norm = flat_recon.cpu().numpy().reshape(self.window_size, len(self.features))

        per_signal_mse = np.mean((windowNorm - recon_norm) ** 2, axis=0)
        total_score = float(np.mean(per_signal_mse))
        return total_score, per_signal_mse, recon_norm

    # Backward-compatible alias
    compute_window_error = computeWindowError

    @staticmethod
    def _signalState(mse: float, q_lo: float, q_hi: float) -> str:
        """3-state classification of one signal against its own bounds.

        Infinite bounds (too few calibration samples) never flag — the
        signal stays ``normal`` rather than faking a guarantee. NaN input
        (e.g. unknown-actual mode category) likewise stays ``normal``; the
        unknown/escalation path owns it.
        """
        if not np.isfinite(mse):
            return "normal" if np.isnan(mse) else "abnormal"
        if not np.isfinite(q_lo):
            return "normal"
        if not np.isfinite(q_lo):
            return "normal"
        if mse <= q_lo:
            return "normal"
        hi = q_hi if np.isfinite(q_hi) else float("inf")
        return "uncertain" if mse <= hi else "abnormal"

    @staticmethod
    def _normalisedExceedance(mse: float, q: float) -> float:
        """MSE relative to its bound; ``inf`` bounds contribute 0 (no flag)."""
        if not np.isfinite(q):
            return 0.0
        if q <= 0.0:
            return 0.0 if mse <= 0.0 else float("inf")
        return float(mse / q)

    def _encodeRawContext(
        self, rawContext: Any
    ) -> Tuple[np.ndarray, bool, List[str], np.ndarray]:
        """Encode a raw context window (W, n_ctx) -> (encoded (W, C), unknown, mode_key, raw)."""
        raw = extractRawContextColumns(rawContext, self.context_features)
        if raw.shape[0] != self.window_size:
            raise ValueError(
                f"Expected context window rows {self.window_size}, got {raw.shape[0]}"
            )
        encoded, unknown_rows = encodeContextMatrix(raw, self.context_features, self.context_specs)
        window_unknown = bool(np.any(unknown_rows))
        mode_key = _modeKeyFromEncoded(encoded[-1] if len(encoded) else np.zeros(0), self.context_layout, self.context_specs)
        return encoded, window_unknown, mode_key, raw

    def _stratifiedNorm(
        self, raw_window: np.ndarray, raw_ctx_rows: Any = None
    ) -> Tuple[np.ndarray, bool]:
        """Standardise one window by its rows' strata; returns (normed, fell_back).

        Rows whose stratum is unknown or under-sampled use global stats and
        set ``fell_back``. With no strata configured this is the global scaler.
        """
        if not self.strata_stats or not self.stratify_fields or raw_ctx_rows is None:
            return self.scaler.transform(raw_window), False
        raw = (
            raw_ctx_rows
            if isinstance(raw_ctx_rows, np.ndarray)
            else extractRawContextColumns(raw_ctx_rows, self.context_features)
        )
        keys = contextRowKeys(raw, self.context_features, self.context_specs,
                              fields=self.stratify_fields)
        scaled, fallback = applyStratifiedStats(
            raw_window, keys, self.strata_stats,
            {"mean": self.scaler.mean.tolist(), "std": self.scaler.std.tolist()},
        )
        return scaled, bool(np.any(fallback))

    @torch.no_grad() if HAS_TORCH else lambda f: f  # type: ignore
    def _scoreModeWindow(
        self, norm_window: np.ndarray, ctx_window: np.ndarray
    ) -> Tuple[Dict[str, float], Dict[str, str]]:
        """Score one window's actual last-step modes under predicted distributions.

        Returns ``(nlls, predicted_names)``. Fields whose actual category is
        unknown are skipped (the unknown flag owns them).
        """
        import torch as _torch

        cont = _torch.from_numpy(np.asarray(norm_window, dtype=np.float32).reshape(1, -1)).float().to(self.device)
        logits = self.mode_model(cont)
        enc_last = np.asarray(ctx_window, dtype=np.float32)[-1:]
        targets, valid = _modeTargets(enc_last, self.context_layout)
        nlls: Dict[str, float] = {}
        predicted: Dict[str, str] = {}
        for h, entry in enumerate(self.context_layout):
            key = f"h{h}"
            feat = entry["feature"]
            cats = [str(c) for c in self.context_specs.get(feat, {}).get("categories", [])]
            if entry["kind"] == "bool":
                p = float(_torch.sigmoid(logits[key][0, 0]).cpu())
                predicted[feat] = "true" if p >= 0.5 else "false"
                t = float(targets[key][0])
                eps = 1e-12
                nlls[feat] = -(t * np.log(max(p, eps)) + (1 - t) * np.log(max(1 - p, eps)))
            else:
                probs = _torch.softmax(logits[key][0], dim=0).cpu().numpy()
                idx = int(np.argmax(probs))
                predicted[feat] = cats[idx] if idx < len(cats) else "unknown"
                if valid[key][0]:
                    t = int(targets[key][0])
                    nlls[feat] = float(-np.log(max(float(probs[t]), 1e-12)))
        return nlls, predicted

    def detect(self, rawWindow: np.ndarray, rawContext: Any = None) -> Dict[str, Any]:
        """Evaluate a raw engineering-unit telemetry window with multi-dimensional attribution.

        ``rawContext`` carries the discrete operating mode over the same
        window (DataFrame/dict/ndarray of Bool/#ENUM raw values); required
        exactly when the detector was configured with context features.
        """
        raw_window = np.asarray(rawWindow, dtype=np.float32)
        if raw_window.shape != (self.window_size, len(self.features)):
            raise ValueError(
                f"Expected window shape ({self.window_size}, {len(self.features)}), got {raw_window.shape}"
            )

        context_mode: List[str] = []
        unseen_mode = False
        context_unknown = False
        stratum_fallback = False
        ctx_window = None
        _raw_ctx_rows = None
        if self.context_dim > 0:
            if rawContext is None:
                raise ValueError(
                    f"Detector expects discrete context {self.context_features}; "
                    "pass rawContext."
                )
            ctx_window, context_unknown, context_mode, _raw_ctx_rows = self._encodeRawContext(rawContext)
            if self.seen_modes is not None and tuple(context_mode) not in self.seen_modes:
                unseen_mode = True
        elif rawContext is not None:
            raise ValueError(
                "Detector was trained without discrete context; rawContext is unexpected."
            )

        norm_window, stratum_fallback = self._stratifiedNorm(
            raw_window, _raw_ctx_rows if self.context_dim > 0 else None
        )
        if stratum_fallback and not unseen_mode:
            unseen_mode = True
        score, per_signal_mse, _ = self.computeWindowError(norm_window, ctx_window)

        # 3-State Conformal Uncertainty (window-mean bound; legacy path)
        if score <= self.q_alpha:
            mean_state = "normal"
        elif score <= self.q_upper:
            mean_state = "uncertain"
        else:
            mean_state = "abnormal"

        # Per-signal bounds defeat mean-dilution: a fault confined to one
        # of F signals moves the mean by only ~1/F, so the global bound can
        # miss it entirely while the culprit signal's own bound fires.
        per_signal_states: Dict[str, str] = {}
        exceedances: Dict[str, float] = {}
        if self.q_alpha_per_signal is not None:
            for idx, feat in enumerate(self.features):
                st = self._signalState(
                    float(per_signal_mse[idx]),
                    float(self.q_alpha_per_signal[idx]),
                    float(self.q_upper_per_signal[idx]),
                )
                per_signal_states[feat] = st
                exceedances[feat] = self._normalisedExceedance(
                    float(per_signal_mse[idx]),
                    float(self.q_alpha_per_signal[idx]),
                )
        abnormal_signals = sorted(
            [f for f, st in per_signal_states.items() if st == "abnormal"]
        )
        uncertain_signals = sorted(
            [f for f, st in per_signal_states.items() if st == "uncertain"]
        )
        # Global state is the worst of the mean-state and any per-signal
        # state, so a previously-abnormal window stays abnormal (back-compat)
        # while a diluted single-sensor fault now escalates through its own
        # signal bound. An unseen operating mode escalates to at least
        # ``uncertain`` — new regime, not proof of fault.
        _rank = {"normal": 0, "uncertain": 1, "abnormal": 2}
        state = mean_state
        for st in per_signal_states.values():
            if _rank[st] > _rank[state]:
                state = st
        if (unseen_mode or context_unknown) and _rank[state] < _rank["uncertain"]:
            state = "uncertain"
        score_max = max(exceedances.values()) if exceedances else 0.0

        # Physics->mode head: which mode does the telemetry imply, and does
        # the switch agree? Classified against per-field conformal NLL bounds
        # and merged worst-of into the global state.
        mode_nlls: Dict[str, float] = {}
        mode_predicted: Dict[str, str] = {}
        mode_states: Dict[str, str] = {}
        if self.mode_model is not None and ctx_window is not None:
            mode_nlls, mode_predicted = self._scoreModeWindow(norm_window, ctx_window)
            for feat, nll in mode_nlls.items():
                q = self.q_mode_per_field.get(feat, float("inf"))
                mode_states[feat] = self._signalState(nll, q, q * self.k_multiplier)
            for st in mode_states.values():
                if _rank[st] > _rank[state]:
                    state = st
        mode_mismatch = sorted([f for f, st in mode_states.items() if st == "abnormal"])

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
            "score_mean": score,  # window-mean MSE (legacy ``score`` alias)
            "score_max": score_max,  # max per-signal MSE / its bound
            "state": state,
            "mean_state": mean_state,  # window-mean-only verdict
            "per_signal_states": per_signal_states,
            "abnormal_signals": abnormal_signals,
            "uncertain_signals": uncertain_signals,
            "per_signal_thresholds": (
                {f: float(self.q_alpha_per_signal[i]) for i, f in enumerate(self.features)}
                if self.q_alpha_per_signal is not None
                else {}
            ),
            "context_features": list(self.context_features),
            "context_mode": context_mode,
            "predicted_mode": mode_predicted,
            "mode_nlls": mode_nlls,
            "mode_states": mode_states,
            "mode_mismatch": mode_mismatch,
            "unseen_mode": unseen_mode,
            "context_unknown": context_unknown,
            "stratum_fallback": stratum_fallback,
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
        rawContexts: Any = None,
        batchSize: int = 256,
    ) -> Dict[str, Any]:
        """High-throughput vectorized batch detection accelerated on GPU/CPU.

        ``rawContexts`` carries discrete operating modes aligned with
        ``rawWindows`` (``(N, W, n_ctx)`` raw values); required exactly when
        the detector was configured with context features. A legacy
        positional ``detectBatch(windows, batchSize)`` int second argument
        is still honoured.
        """
        if isinstance(rawContexts, (int, np.integer)):
            batchSize = int(rawContexts)
            rawContexts = None
        raw_windows = np.asarray(rawWindows, dtype=np.float32)
        n_windows, w_size, n_feats = raw_windows.shape
        if w_size != self.window_size or n_feats != len(self.features):
            raise ValueError(
                f"Expected shape (*, {self.window_size}, {len(self.features)}), got {raw_windows.shape}"
            )

        ctx_all = None
        ctx_unknown_all = np.zeros(n_windows, dtype=bool)
        mode_keys: List[List[str]] = [[] for _ in range(n_windows)]
        if self.context_dim > 0:
            if rawContexts is None:
                raise ValueError(
                    f"Detector expects discrete context {self.context_features}; "
                    "pass rawContexts."
                )
            raw_ctx = rawContexts
            if HAS_PANDAS and isinstance(raw_ctx, pd.DataFrame):
                raw_ctx = extractRawContextColumns(raw_ctx, self.context_features)
            elif isinstance(raw_ctx, dict):
                raw_ctx = extractRawContextColumns(raw_ctx, self.context_features)
            else:
                raw_ctx = np.asarray(raw_ctx, dtype=object)
            if raw_ctx.shape[:2] != (n_windows, self.window_size):
                raise ValueError(
                    f"Expected contexts shape ({n_windows}, {self.window_size}, n_ctx), "
                    f"got {raw_ctx.shape}"
                )
            flat_raw = raw_ctx.reshape(n_windows * self.window_size, raw_ctx.shape[2])
            enc_flat, unknown_flat = encodeContextMatrix(
                flat_raw, self.context_features, self.context_specs
            )
            ctx_all = enc_flat.reshape(
                n_windows, self.window_size, self.context_dim
            ).astype(np.float32)
            ctx_unknown_all = unknown_flat.reshape(n_windows, self.window_size).any(axis=1)
            for w in range(n_windows):
                mode_keys[w] = _modeKeyFromEncoded(ctx_all[w, -1], self.context_layout, self.context_specs)
        elif rawContexts is not None:
            raise ValueError(
                "Detector was trained without discrete context; rawContexts is unexpected."
            )

        norm_windows = np.empty_like(raw_windows)
        stratum_fallback_all = np.zeros(n_windows, dtype=bool)
        if self.context_dim > 0 and self.strata_stats and self.stratify_fields:
            for w in range(n_windows):
                norm_windows[w], stratum_fallback_all[w] = self._stratifiedNorm(
                    raw_windows[w], raw_ctx[w]
                )
        else:
            norm_windows = (raw_windows - self.scaler.mean) / self.scaler.std
        flat_all = np.concatenate(
            [norm_windows.reshape(n_windows, self.window_size * len(self.features))]
            + ([ctx_all.reshape(n_windows, self.window_size * self.context_dim)]
               if ctx_all is not None else []),
            axis=1,
        )

        scores = np.zeros(n_windows, dtype=np.float32)
        per_sig_all = np.zeros((n_windows, len(self.features)), dtype=np.float32)

        for start_idx in range(0, n_windows, batchSize):
            end_idx = min(n_windows, start_idx + batchSize)
            batch_flat = torch.from_numpy(flat_all[start_idx:end_idx]).float().to(self.device)
            batch_recon = self.model(batch_flat)

            b_size = end_idx - start_idx
            cont_width = self.window_size * len(self.features)
            b_in = batch_flat[:, :cont_width].view(b_size, self.window_size, len(self.features))
            b_out = batch_recon.view(b_size, self.window_size, len(self.features))

            b_sig_mse = torch.mean((b_in - b_out) ** 2, dim=1)
            b_total_score = torch.mean(b_sig_mse, dim=1)

            scores[start_idx:end_idx] = b_total_score.cpu().numpy()
            per_sig_all[start_idx:end_idx] = b_sig_mse.cpu().numpy()

        states = []
        per_signal_states_all: List[Dict[str, str]] = []
        abnormal_signals_all: List[List[str]] = []
        unseen_modes_all: List[bool] = []
        mode_nlls_all: List[Dict[str, float]] = []
        mode_mismatch_all: List[List[str]] = []
        if self.q_alpha_per_signal is not None:
            q_lo = np.asarray(self.q_alpha_per_signal, dtype=np.float64)
            q_hi = np.asarray(self.q_upper_per_signal, dtype=np.float64)
        mode_batch_nlls: Dict[str, List[float]] = {}
        if self.mode_model is not None and ctx_all is not None:
            mode_batch_nlls = modeNLLs(
                self.mode_model, norm_windows, ctx_all,
                self.context_layout, self.device, batch_size=batchSize,
            )
        _rank = {"normal": 0, "uncertain": 1, "abnormal": 2}
        for w, s in enumerate(scores):
            if s <= self.q_alpha:
                mean_state = "normal"
            elif s <= self.q_upper:
                mean_state = "uncertain"
            else:
                mean_state = "abnormal"
            unseen = False
            if self.context_dim > 0:
                unseen = bool(ctx_unknown_all[w]) or bool(stratum_fallback_all[w]) or (
                    self.seen_modes is not None
                    and tuple(mode_keys[w]) not in self.seen_modes
                )
            unseen_modes_all.append(unseen)
            if self.q_alpha_per_signal is not None:
                row = per_sig_all[w]
                sig_states = {
                    self.features[j]: self._signalState(
                        float(row[j]), float(q_lo[j]), float(q_hi[j])
                    )
                    for j in range(len(self.features))
                }
                worst = mean_state
                for st in sig_states.values():
                    if _rank[st] > _rank[worst]:
                        worst = st
                if unseen and _rank[worst] < _rank["uncertain"]:
                    worst = "uncertain"
                states.append(worst)
                per_signal_states_all.append(sig_states)
                abnormal_signals_all.append(
                    sorted([f for f, st in sig_states.items() if st == "abnormal"])
                )
            else:
                if unseen and _rank[mean_state] < _rank["uncertain"]:
                    mean_state = "uncertain"
                states.append(mean_state)
                per_signal_states_all.append({})
                abnormal_signals_all.append([])
            if mode_batch_nlls:
                wnll = {f: mode_batch_nlls[f][w] for f in mode_batch_nlls}
                mode_nlls_all.append(wnll)
                mism = sorted(
                    f for f, v in wnll.items()
                    if self._signalState(v, self.q_mode_per_field.get(f, float("inf")),
                                         self.q_mode_per_field.get(f, float("inf")) * self.k_multiplier) == "abnormal"
                )
                mode_mismatch_all.append(mism)
                for f in mism:
                    if _rank["abnormal"] > _rank[states[w]]:
                        states[w] = "abnormal"
            else:
                mode_nlls_all.append({})
                mode_mismatch_all.append([])

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
            "per_signal_states": per_signal_states_all,
            "abnormal_signals": abnormal_signals_all,
            "mode_nlls": mode_nlls_all,
            "mode_mismatch": mode_mismatch_all,
            "unseen_modes": unseen_modes_all,
            "context_modes": mode_keys,
            "primary_dimensions": primary_dimensions,
            "primary_subsystems": primary_subsystems,
            "per_signal_mse": per_sig_all,
            "top_signals": top_signals,
            "q_alpha": self.q_alpha,
            "q_upper": self.q_upper,
            "q_alpha_per_signal": (
                np.asarray(self.q_alpha_per_signal, dtype=np.float64).copy()
                if self.q_alpha_per_signal is not None
                else None
            ),
        }

    # Backward-compatible alias
    detect_batch = detectBatch

    def predict(self, rawWindow: np.ndarray, rawContext: Any = None) -> Dict[str, Any]:
        """Canonical inference entry point; identical to :meth:`detect`."""
        return self.detect(rawWindow, rawContext)

    def explain(self, rawWindow: np.ndarray, rawContext: Any = None,
                top_k: int = 3) -> Dict[str, Any]:
        """Engineering-unit-aware evidence report for one window.

        Joins the detect() verdict with original physical measurements
        (inverse-standardised via the fitted scaler), declared units from
        the schema ``dimension`` taxonomy, and the applied transform, so a
        reader can tell standardized model space apart from plant units.
        Reconstruction shares and mode disagreements are correlational
        evidence — they rank suspects, they do not prove root cause.
        """
        verdict = self.detect(rawWindow, rawContext)
        raw_window = np.asarray(rawWindow, dtype=np.float32)
        signals = []
        for entry in verdict.get("top_signals", [])[: max(0, top_k)]:
            signals.append({
                "signal": entry.get("signal"),
                "subsystem": entry.get("subsystem"),
                "unit_dimension": entry.get("unit_dimension"),
                "current_value": entry.get("current_value"),
                "normal_mean": entry.get("normal_mean"),
                "normal_range": entry.get("normal_range"),
                "deviation_delta": entry.get("deviation_delta"),
                "reconstruction_error": entry.get("reconstruction_error"),
            })
        return {
            "state": verdict.get("state"),
            "score": verdict.get("score"),
            "threshold": verdict.get("q_alpha"),
            "upper_threshold": verdict.get("q_upper"),
            "primary_subsystem": verdict.get("primary_subsystem"),
            "primary_dimension": verdict.get("primary_dimension"),
            "suspect_signals": signals,
            "mode_mismatch": verdict.get("mode_mismatch", []),
            "predicted_mode": verdict.get("predicted_mode", {}),
            "context_mode": verdict.get("context_mode", []),
            "unseen_mode": verdict.get("unseen_mode", False),
            "window_shape": [int(v) for v in np.shape(raw_window)],
            "transform": "z-score standardisation fitted on nominal training rows "
                         "(see scaler/feature ordering in the saved bundle)",
            "note": ("Reconstruction errors and mode disagreements are "
                     "correlational evidence ranking suspect signals; they do "
                     "not establish a causal root cause."),
        }

    def save(self, saveDir: Union[str, Path],
             trainConfig: Optional[Dict[str, Any]] = None) -> None:
        """Save PyTorch weights and calibration metadata to disk.

        The bundle records architecture, preprocessing, feature identity and
        ordering, schema dimensions, training/calibration provenance and
        package versions — everything :meth:`load` needs to reproduce
        inference and to reject incompatible inputs. No telemetry rows are
        stored. Pass ``trainConfig`` (epochs/seed/alpha/row counts) for
        provenance; anything JSON-serializable is kept verbatim.
        """
        import datetime as _dt

        save_dir = Path(saveDir)
        save_dir.mkdir(parents=True, exist_ok=True)
        torch.save(self.model.state_dict(), save_dir / "model.pt")
        if HAS_TORCH and isinstance(getattr(self, "mode_model", None), nn.Module):
            torch.save(self.mode_model.state_dict(), save_dir / "mode_model.pt")
        try:
            import sgrn as _sgrn

            pkg_version = getattr(_sgrn, "__version__", "unknown")
        except Exception:
            pkg_version = "unknown"
        try:
            torch_version = torch.__version__
        except Exception:
            torch_version = "unknown"
        meta = {
            "format": "sgrn-manifold-bundle/1",
            "sgrn_version": pkg_version,
            "torch_version": torch_version,
            "created_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
            "train_config": dict(trainConfig or {}),
            "window_size": self.window_size,
            "input_dim": self.window_size * len(self.features) + self.window_size * self.context_dim,
            "cont_dim": self.window_size * len(self.features),
            "context_dim": self.context_dim,
            "hidden_dim": getattr(self.model, "hidden_dim", 64),
            "latent_dim": getattr(self.model, "latent_dim", 16),
            "features": self.features,
            "feature_dimensions": self.feature_dimensions,
            "feature_subsystems": self.feature_subsystems,
            "context_features": self.context_features,
            "context_specs": self.context_specs,
            "seen_modes": sorted([list(m) for m in self.seen_modes]) if self.seen_modes is not None else None,
            "stratify_fields": self.stratify_fields,
            "strata_stats": self.strata_stats,
            "mode_hidden": getattr(getattr(self, "mode_model", None), "hidden_dim", None),
            "q_mode_per_field": {f: float(q) for f, q in self.q_mode_per_field.items()},
            "calibration_stride": self.calibration_stride,
            "q_alpha_per_signal": (
                np.asarray(self.q_alpha_per_signal, dtype=np.float64).tolist()
                if self.q_alpha_per_signal is not None
                else None
            ),
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
        expectedFeatures: Optional[List[str]] = None,
    ) -> ProcessManifoldDetector:
        """Load trained model weights and conformal calibration metadata from directory.

        Validates feature identity and ordering (and the scaler ordering)
        against the bundle, and against ``expectedFeatures`` when given —
        a renamed, reordered or resized feature set raises instead of
        scoring silently wrong. Unknown bundle ``format`` values warn.
        """
        save_dir = Path(saveDir)
        meta = json.loads((save_dir / "model_meta.json").read_text(encoding="utf-8"))
        if meta.get("format", "sgrn-manifold-bundle/0") != "sgrn-manifold-bundle/1":
            import warnings

            warnings.warn(
                f"Unknown bundle format {meta.get('format')!r}; "
                "inference compatibility is not guaranteed.", UserWarning, stacklevel=2)
        scaler = Scaler.fromDict(meta["scaler"])
        features = meta["features"]
        if expectedFeatures is not None and list(expectedFeatures) != list(features):
            raise ValueError(
                f"Feature mismatch: caller expects {list(expectedFeatures)} "
                f"but bundle holds {list(features)}")
        if list(scaler.feature_names) != list(features):
            raise ValueError(
                "Bundle scaler ordering does not match bundle features; "
                "refusing to load an inconsistent artifact.")
        feature_dimensions = meta.get("feature_dimensions")
        feature_subsystems = meta.get("feature_subsystems")
        window_size = meta["window_size"]
        input_dim = meta["input_dim"]

        hidden_dim = 64
        latent_dim = 16
        if config and "model" in config:
            hidden_dim = config["model"].get("hidden_dim", 64)
            latent_dim = config["model"].get("latent_dim", 16)
        else:
            # Fall back to the dims persisted at save time so non-default
            # architectures reload without a state-dict shape mismatch.
            hidden_dim = meta.get("hidden_dim", 64)
            latent_dim = meta.get("latent_dim", 16)

        target_device = resolveDevice(device)
        ctx_dim_ts = int(meta.get("context_dim", 0) or 0)
        # WindowAutoencoder.context_dim is the flattened extra width; the
        # persisted detector-level context_dim is per-timestep.
        ctx_flat = window_size * ctx_dim_ts if ctx_dim_ts else 0
        cont_dim = meta.get("cont_dim", input_dim - ctx_flat if ctx_flat else input_dim)
        model = WindowAutoencoder(input_dim=input_dim, hidden_dim=hidden_dim, latent_dim=latent_dim,
                                  context_dim=ctx_flat, cont_dim=cont_dim)
        model.load_state_dict(torch.load(save_dir / "model.pt", map_location=target_device, weights_only=True))
        model.to(target_device)
        model.eval()

        mode_model = None
        mode_path = save_dir / "mode_model.pt"
        if mode_path.exists() and ctx_dim_ts:
            mode_hidden = meta.get("mode_hidden") or 32
            mode_layout = contextLayout(meta.get("context_features") or [],
                                        meta.get("context_specs") or {})
            mode_model = ModePredictor(cont_dim=cont_dim, layout=mode_layout,
                                       hidden_dim=int(mode_hidden))
            mode_model.load_state_dict(torch.load(mode_path, map_location=target_device, weights_only=True))
            mode_model.to(target_device)
            mode_model.eval()

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
            q_alpha_per_signal=meta.get("q_alpha_per_signal"),
            calibration_stride=meta.get("calibration_stride"),
            context_features=meta.get("context_features"),
            context_specs=meta.get("context_specs"),
            seen_modes=meta.get("seen_modes"),
            stratify_fields=meta.get("stratify_fields"),
            strata_stats=meta.get("strata_stats"),
            mode_model=mode_model,
            q_mode_per_field=meta.get("q_mode_per_field"),
        )


# ======================================================================================
# GPU-Accelerated Model Training & Split-Conformal Calibration Loop
# ======================================================================================

def _conformalQuantile(scores: List[float], alpha: float) -> float:
    """Finite-sample split-conformal quantile with ``inf`` fallback.

    ``k = ceil((n+1)(1-alpha))``; ``q = sorted[k-1]``, or ``+inf`` when
    ``k > n`` (too few samples to guarantee coverage — never flag rather
    than fake a guarantee). No interpolation, no clamping to the max.
    """
    import math as _math

    n = len(scores)
    if n == 0:
        return float("inf")
    k = int(_math.ceil((n + 1) * (1.0 - alpha)))
    if k > n:
        return float("inf")
    return float(sorted(scores)[k - 1])


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
    calibrationStride: Optional[int] = None,
    calibration_stride: Optional[int] = None,
    X_ctx_train: Optional[np.ndarray] = None,
    X_ctx_cal: Optional[np.ndarray] = None,
    context_features: Optional[List[str]] = None,
    context_specs: Optional[Dict[str, Any]] = None,
    stratify_fields: Optional[List[str]] = None,
    strata_stats: Optional[Dict[str, Any]] = None,
    trainModeHead: bool = True,
    modeHeadConfig: Optional[Dict[str, Any]] = None,
) -> ProcessManifoldDetector:
    """Train manifold autoencoder on training split and compute conformal threshold on calibration split.

    Calibration uses non-overlapping windows: with stride-1 sliding windows,
    consecutive scores share W-1 timesteps and violate the exchangeability
    the conformal guarantee needs. ``calibrationStride`` thins
    ``X_cal`` to ``X_cal[::calibrationStride]`` before scoring (pass the
    window length for disjoint blocks; ``1`` reproduces the legacy
    overlapping behaviour). Fewer effective samples can push small
    calibration sets into the ``inf`` (never-flag) regime — that is the
    honest price of the guarantee, reported via ``n_cal_effective``.
    Per-signal bounds are calibrated alongside the window-mean bound so
    single-sensor faults are not diluted away (see
    :meth:`ProcessManifoldDetector.detect`).

    Discrete operating context (``X_ctx_*`` one-hot windows + specs)
    conditions the encoder without entering the reconstruction loss: the
    same continuous physics can score differently per mode, and unseen
    modes escalate to ``uncertain`` at detect time.
    """
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
    cont_dim = window_size * num_features

    ctx_features: List[str] = list(context_features or [])
    ctx_specs: Dict[str, Any] = dict(context_specs or {})
    ctx_layout = contextLayout(ctx_features, ctx_specs)
    ctx_dim = sum(e["width"] for e in ctx_layout) if ctx_layout else 0
    if ctx_features and (X_ctx_train is None or X_ctx_cal is None):
        raise ValueError(
            "context_features given but X_ctx_train/X_ctx_cal missing; "
            "pass encoded context windows or leave context empty."
        )
    if X_ctx_train is not None and len(X_ctx_train) != num_windows:
        raise ValueError(
            f"X_ctx_train rows {len(X_ctx_train)} != X_train windows {num_windows}"
        )
    if X_ctx_train is not None and X_ctx_train.shape[1:] != (window_size, ctx_dim):
        raise ValueError(
            f"Expected X_ctx_train shape (*, {window_size}, {ctx_dim}), "
            f"got {X_ctx_train.shape}"
        )
    input_dim = cont_dim + (window_size * ctx_dim if ctx_dim else 0)

    X_train_flat = torch.from_numpy(
        np.concatenate(
            [X_train.reshape(num_windows, cont_dim)]
            + ([X_ctx_train.reshape(num_windows, window_size * ctx_dim)]
               if ctx_dim else []),
            axis=1,
        )
    ).float()
    dataset = TensorDataset(X_train_flat, X_train_flat[:, :cont_dim])
    loader = DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=True,
        pin_memory=is_cuda,
    )

    model = WindowAutoencoder(input_dim=input_dim, hidden_dim=hidden_dim, latent_dim=latent_dim,
                              context_dim=(window_size * ctx_dim if ctx_dim else 0),
                              cont_dim=cont_dim)
    model.to(target_device)
    optimizer = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=1e-5)
    criterion = nn.MSELoss()

    model.train()
    for epoch in range(epochs):
        epoch_loss = 0.0
        for batch_x, batch_y in loader:
            batch_x = batch_x.to(target_device, non_blocking=is_cuda)
            batch_y = batch_y.to(target_device, non_blocking=is_cuda)
            optimizer.zero_grad()
            recon = model(batch_x)
            loss = criterion(recon, batch_y)
            loss.backward()
            optimizer.step()
            epoch_loss += loss.item() * len(batch_x)
        epoch_loss /= len(X_train_flat)
        if (epoch + 1) % 10 == 0 or epoch == epochs - 1:
            print(f"[manifold_train] Epoch {epoch + 1:2d}/{epochs:2d} — MSE Loss: {epoch_loss:.5f}")

    model.eval()

    # Split-Conformal Calibration on held-out X_cal, thinned to
    # non-overlapping blocks for approximate exchangeability.
    if calibrationStride is None:
        calibrationStride = calibration_stride
    cal_stride = calibrationStride if calibrationStride is not None else window_size
    cal_stride = max(1, int(cal_stride))
    X_cal_eff = X_cal[::cal_stride] if len(X_cal) else X_cal
    X_ctx_eff = X_ctx_cal[::cal_stride] if X_ctx_cal is not None and len(X_ctx_cal) else X_ctx_cal
    n_cal = len(X_cal_eff)
    X_cal_flat = (
        torch.from_numpy(X_cal_eff.reshape(n_cal, cont_dim)).float()
        if n_cal
        else torch.empty((0, cont_dim))
    )
    if ctx_dim and X_ctx_eff is not None:
        X_ctx_flat = torch.from_numpy(
            X_ctx_eff.reshape(n_cal, window_size * ctx_dim)
        ).float()
    else:
        X_ctx_flat = None
    cal_scores: List[float] = []
    cal_sig_scores: List[List[float]] = []

    with torch.no_grad():
        cal_batch_size = min(256, n_cal) if n_cal else 0
        for i in range(0, n_cal, cal_batch_size or 1):
            b_cal = X_cal_flat[i : i + cal_batch_size].to(target_device, non_blocking=is_cuda)
            if X_ctx_flat is not None:
                b_ctx = X_ctx_flat[i : i + cal_batch_size].to(target_device, non_blocking=is_cuda)
                b_in = torch.cat([b_cal, b_ctx], dim=1)
            else:
                b_in = b_cal
            b_recon = model(b_in)
            b_errs = torch.mean((b_cal - b_recon) ** 2, dim=1).cpu().numpy()
            cal_scores.extend(float(v) for v in b_errs)
            b_in = b_cal.view(len(b_cal), window_size, num_features)
            b_out = b_recon.view(len(b_cal), window_size, num_features)
            b_sig = torch.mean((b_in - b_out) ** 2, dim=1).cpu().numpy()
            cal_sig_scores.extend([float(v) for v in row] for row in b_sig)

    if X_ctx_cal is not None and len(X_ctx_cal) != len(X_cal):
        raise ValueError(
            f"X_ctx_cal rows {len(X_ctx_cal)} != X_cal windows {len(X_cal)}"
        )

    q_alpha = _conformalQuantile(cal_scores, alpha)
    q_per_signal = [
        _conformalQuantile([row[j] for row in cal_sig_scores], alpha)
        for j in range(num_features)
    ]

    # Nominal operating modes (last-step category per context field) seen in
    # training. Detection escalates unseen modes to at least ``uncertain``.
    seen_modes: Optional[List[List[str]]] = None
    if ctx_dim and X_ctx_train is not None:
        seen: set = set()
        for w in range(len(X_ctx_train)):
            key = _modeKeyFromEncoded(X_ctx_train[w, -1], ctx_layout, ctx_specs)
            if "unknown" not in key:
                seen.add(tuple(key))
        seen_modes = sorted([list(m) for m in seen])

    print(f"[conformal] Calibration windows evaluated: {n_cal} "
          f"(from {len(X_cal)} at stride {cal_stride})")
    print(f"[conformal] q_{alpha:.2f} threshold (nominal {100*(1-alpha):.0f}% coverage): {q_alpha:.5f}")
    print(f"[conformal] Uncertain upper threshold ({k_mult}x multiplier): {q_alpha * k_mult:.5f}")
    print(f"[conformal] Per-signal bounds: min={min(q_per_signal):.5f} "
          f"max={max(q_per_signal):.5f} "
          f"inf_count={sum(1 for q in q_per_signal if not np.isfinite(q))}/{len(q_per_signal)}")
    if ctx_dim:
        print(f"[conformal] Context: {len(ctx_features)} signals, "
              f"{ctx_dim} one-hot dims, {len(seen_modes or [])} nominal modes")

    # Physics->mode head: same nominal windows, disjoint target. Disabled
    # without context (nothing to predict) or on explicit opt-out.
    mode_model = None
    q_mode: Dict[str, float] = {}
    if trainModeHead and ctx_dim and X_ctx_train is not None and len(X_ctx_train):
        mh_cfg = dict(modeHeadConfig or {})
        mode_model = trainModePredictor(
            X_train, X_ctx_train, ctx_layout,
            hidden_dim=int(mh_cfg.get("hidden_dim", 32)),
            epochs=int(mh_cfg.get("epochs", 20)),
            batch_size=int(mh_cfg.get("batch_size", 64)),
            lr=float(mh_cfg.get("learning_rate", 1e-3)),
            seed=seed,
            device=target_device,
        )
        if n_cal and X_ctx_eff is not None:
            cal_nlls = modeNLLs(mode_model, X_cal_eff, X_ctx_eff, ctx_layout,
                                target_device)
            for feat, vals in cal_nlls.items():
                finite = [v for v in vals if np.isfinite(v)]
                q_mode[feat] = _conformalQuantile(finite, alpha)
            n_flag = sum(1 for q in q_mode.values() if np.isfinite(q))
            print(f"[conformal] Mode head bounds: {n_flag}/{len(q_mode)} finite")

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
        q_alpha_per_signal=q_per_signal,
        calibration_stride=cal_stride,
        context_features=ctx_features,
        context_specs=ctx_specs,
        seen_modes=seen_modes,
        stratify_fields=stratify_fields,
        strata_stats=strata_stats,
        mode_model=mode_model,
        q_mode_per_field=q_mode,
    )


# Backward-compatible snake_case alias
train_manifold_detector = trainManifoldDetector


def _modeTargets(
    enc_last: np.ndarray, layout: List[Dict[str, Any]]
) -> Tuple[Dict[str, np.ndarray], Dict[str, np.ndarray]]:
    """Last-step encoded context -> per-head targets + valid masks.

    Bool -> float32 0/1; enum -> int64 class index with -1 for unknown rows
    (excluded from loss via ``ignore_index``).
    """
    bool_cols, enum_cols, enum_valid = [], [], []
    for entry in layout:
        seg = enc_last[:, entry["offset"] : entry["offset"] + entry["width"]]
        if entry["kind"] == "bool":
            bool_cols.append((seg[:, 0] >= 0.5).astype(np.float32))
        else:
            k = entry["width"] - 1
            idx = np.argmax(seg, axis=1)
            valid = idx < k
            idx = np.where(valid, idx, -1).astype(np.int64)
            enum_cols.append(idx)
            enum_valid.append(valid)
    out_t, out_v = {}, {}
    bi = ei = 0
    for h, entry in enumerate(layout):
        key = f"h{h}"
        if entry["kind"] == "bool":
            out_t[key] = bool_cols[bi]
            out_v[key] = np.ones_like(bool_cols[bi], dtype=bool)
            bi += 1
        else:
            out_t[key] = enum_cols[ei]
            out_v[key] = enum_valid[ei]
            ei += 1
    return out_t, out_v


def trainModePredictor(
    X_train: np.ndarray,
    X_ctx_train: np.ndarray,
    layout: List[Dict[str, Any]],
    hidden_dim: int = 32,
    epochs: int = 20,
    batch_size: int = 64,
    lr: float = 1e-3,
    seed: int = 42,
    device: Optional[Union[str, Any]] = None,
) -> Any:
    """Train physics->mode predictor on nominal windows (context never an input)."""
    if not HAS_TORCH:
        raise ImportError("PyTorch is required to train the mode predictor.")
    import torch as _torch

    _torch.manual_seed(seed)
    target_device = resolveDevice(device)
    n, window_size = X_train.shape[0], X_train.shape[1]
    cont_dim = int(np.prod(X_train.shape[1:]))
    Xc = torch.from_numpy(X_train.reshape(n, cont_dim)).float()
    enc_last = np.asarray(X_ctx_train)[:, -1, :].reshape(n, -1)
    targets, valid = _modeTargets(enc_last, layout)

    model = ModePredictor(cont_dim=cont_dim, layout=layout, hidden_dim=hidden_dim)
    model.to(target_device)
    model.train()
    opt = _torch.optim.Adam(model.parameters(), lr=lr)
    bce = nn.BCEWithLogitsLoss(reduction="none")
    ce = nn.CrossEntropyLoss(reduction="none", ignore_index=-1)
    is_bool = {f"h{h}": (e["kind"] == "bool") for h, e in enumerate(layout)}

    idx = np.arange(n)
    for _ in range(max(0, epochs)):
        np.random.shuffle(idx)
        for s in range(0, n, batch_size):
            b = idx[s : s + batch_size]
            xb = Xc[b].to(target_device)
            outs = model(xb)
            losses = []
            for key, logits in outs.items():
                tb = torch.from_numpy(targets[key][b]).to(target_device)
                if is_bool[key]:
                    losses.append(bce(logits[:, 0], tb).mean())
                else:
                    te = torch.from_numpy(targets[key][b]).long().to(target_device)
                    row = ce(logits, te)
                    losses.append(row.mean())
            opt.zero_grad()
            _torch.stack(losses).mean().backward()
            opt.step()
    model.eval()
    return model


def modeNLLs(
    model: Any,
    X_cont: np.ndarray,
    X_ctx: np.ndarray,
    layout: List[Dict[str, Any]],
    device: Any,
    batch_size: int = 256,
) -> Dict[str, List[float]]:
    """Per-field NLLs of actual last-step modes under predicted distributions."""
    import torch as _torch

    if not HAS_TORCH:
        raise ImportError("PyTorch is required for mode scoring.")
    n = X_cont.shape[0]
    cont_dim = int(np.prod(X_cont.shape[1:]))
    Xc = torch.from_numpy(np.asarray(X_cont).reshape(n, cont_dim)).float()
    enc_last = np.asarray(X_ctx)[:, -1, :].reshape(n, -1)
    targets, valid = _modeTargets(enc_last, layout)
    is_bool = {f"h{h}": (e["kind"] == "bool") for h, e in enumerate(layout)}
    bce = nn.BCEWithLogitsLoss(reduction="none")
    ce = nn.CrossEntropyLoss(reduction="none", ignore_index=-1)
    per_field: Dict[str, List[float]] = {e["feature"]: [] for e in layout}
    feats = [e["feature"] for e in layout]
    model.eval()
    with _torch.no_grad():
        for s in range(0, n, batch_size):
            e = min(n, s + batch_size)
            outs = model(Xc[s:e].to(device))
            for h, key in enumerate(sorted(outs.keys(), key=lambda k: int(k[1:]))):
                feat = feats[h]
                logits = outs[key]
                if is_bool[key]:
                    tb = torch.from_numpy(targets[key][s:e]).to(device)
                    row = bce(logits[:, 0], tb).cpu().numpy()
                    per_field[feat].extend(float(v) for v in row)
                else:
                    te = torch.from_numpy(targets[key][s:e]).long().to(device)
                    row = ce(logits, te).cpu().numpy()
                    mask = valid[key][s:e]
                    # Stay window-aligned: NaN marks unknown-actual rows.
                    per_field[feat].extend(float(v) if m else float("nan") for v, m in zip(row, mask))
    return per_field


# Backward-compatible snake_case aliases
train_manifold_detector = trainManifoldDetector
train_mode_predictor = trainModePredictor
mode_nlls = modeNLLs
