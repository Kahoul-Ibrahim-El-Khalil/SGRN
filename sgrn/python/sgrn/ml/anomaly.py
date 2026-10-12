"""
sgrn.ml.anomaly — PyTorch Autoencoder + Split-Conformal Anomaly & Drift Detector.

Implements uncertainty-aware anomaly detection using split-conformal prediction
to guarantee a user-defined false-alarm rate (1 - alpha) on nominal process baselines.
"""

from typing import Dict, Any, List, Tuple, Optional
import math

try:
    import torch
    import torch.nn as nn
    import torch.optim as optim
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


if HAS_TORCH:
    class AutoencoderNet(nn.Module):
        """Neural autoencoder for industrial process time-series reconstruction."""

        def __init__(self, input_dim: int, hidden_dim: int = 16, latent_dim: int = 8):
            super().__init__()
            self.encoder = nn.Sequential(
                nn.Linear(input_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, latent_dim),
                nn.ReLU()
            )
            self.decoder = nn.Sequential(
                nn.Linear(latent_dim, hidden_dim),
                nn.ReLU(),
                nn.Linear(hidden_dim, input_dim)
            )

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            return self.decoder(self.encoder(x))


class SplitConformalCalibrator:
    """Split-conformal prediction calibrator for anomaly thresholding with statistical guarantees."""

    def __init__(self, alpha: float = 0.05):
        self.alpha = alpha  # Significance level (e.g., 0.05 -> 95% nominal coverage)
        self.quantile_threshold: float = float("inf")
        self.feature_names: List[str] = []

    def calibrate(self, nonconformity_scores: List[float]) -> float:
        """Calibrate quantile threshold q_hat on calibration nonconformity scores."""
        n = len(nonconformity_scores)
        if n == 0:
            self.quantile_threshold = float("inf")
            return self.quantile_threshold

        sorted_scores = sorted(nonconformity_scores)
        # Conformal quantile index: ceil((n + 1) * (1 - alpha)) / n
        q_idx = int(math.ceil((n + 1) * (1 - self.alpha))) - 1
        q_idx = min(max(0, q_idx), n - 1)
        self.quantile_threshold = sorted_scores[q_idx]
        return self.quantile_threshold

    def evaluate_sample(
        self, raw_sample: List[float], recon_sample: List[float]
    ) -> Tuple[bool, float, Dict[str, float]]:
        """Evaluate a single test sample for anomaly status, uncertainty score, and feature attribution."""
        errors = [abs(r - c) for r, c in zip(raw_sample, recon_sample)]
        score = sum(e ** 2 for e in errors) ** 0.5  # L2 nonconformity score

        is_anomaly = score > self.quantile_threshold
        
        # Feature attribution (% of total reconstruction error)
        total_err = sum(errors) if sum(errors) > 0 else 1.0
        attribution = {}
        for idx, err in enumerate(errors):
            feat_name = self.feature_names[idx] if idx < len(self.feature_names) else f"feat_{idx}"
            attribution[feat_name] = round(err / total_err, 4)

        return is_anomaly, score, attribution


class ConformalAnomalyDetector:
    """High-level PyTorch + Split-Conformal Anomaly & Drift Detection Pipeline."""

    def __init__(self, feature_names: List[str], alpha: float = 0.05, latent_dim: int = 8):
        self.feature_names = feature_names
        self.input_dim = len(feature_names)
        self.calibrator = SplitConformalCalibrator(alpha=alpha)
        self.calibrator.feature_names = feature_names

        if HAS_TORCH:
            self.model = AutoencoderNet(input_dim=self.input_dim, latent_dim=latent_dim)
            self.optimizer = optim.Adam(self.model.parameters(), lr=1e-3)
            self.criterion = nn.MSELoss()
        else:
            self.model = None

    def train_baseline(self, train_data: List[List[float]], epochs: int = 50, batch_size: int = 32) -> float:
        """Train autoencoder on normal baseline operating data (e.g. motor start / nominal scan)."""
        if not HAS_TORCH or self.model is None:
            return 0.0

        tensor_data = torch.tensor(train_data, dtype=torch.float32)
        dataset = torch.utils.data.TensorDataset(tensor_data)
        loader = torch.utils.data.DataLoader(dataset, batch_size=batch_size, shuffle=True)

        self.model.train()
        last_loss = 0.0
        for epoch in range(epochs):
            total_loss = 0.0
            for (batch,) in loader:
                self.optimizer.zero_grad()
                recon = self.model(batch)
                loss = self.criterion(recon, batch)
                loss.backward()
                self.optimizer.step()
                total_loss += loss.item() * len(batch)
            last_loss = total_loss / len(train_data)
        return last_loss

    def calibrate_conformal(self, calib_data: List[List[float]]) -> float:
        """Calibrate conformal prediction threshold on calibration data."""
        if not HAS_TORCH or self.model is None:
            # Fallback score if torch not installed
            scores = [sum(x**2 for x in sample)**0.5 for sample in calib_data]
            return self.calibrator.calibrate(scores)

        self.model.eval()
        tensor_data = torch.tensor(calib_data, dtype=torch.float32)
        with torch.no_grad():
            recon = self.model(tensor_data)
            errors = torch.norm(tensor_data - recon, dim=1).numpy().tolist()

        return self.calibrator.calibrate(errors)

    def predict(self, sample: List[float]) -> Tuple[bool, float, Dict[str, float]]:
        """Predict anomaly status, L2 score, and feature-level attribution for a live sample."""
        if not HAS_TORCH or self.model is None:
            score = sum(x**2 for x in sample)**0.5
            is_anomaly = score > self.calibrator.quantile_threshold
            attribution = {name: 1.0 / len(sample) for name in self.feature_names}
            return is_anomaly, score, attribution

        self.model.eval()
        tensor_sample = torch.tensor([sample], dtype=torch.float32)
        with torch.no_grad():
            recon_tensor = self.model(tensor_sample)
            recon = recon_tensor[0].numpy().tolist()

        return self.calibrator.evaluate_sample(sample, recon)
