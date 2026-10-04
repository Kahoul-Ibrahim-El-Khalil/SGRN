"""Reusable anomaly monitoring and fault diagnosis helpers."""

from __future__ import annotations

from typing import Sequence

import numpy as np

__all__ = ["fitMonitorAndDiagnoser", "explainSample"]


def fitMonitorAndDiagnoser(df_train, df_cal, feature_names: Sequence[str]):
    """Fit a normal-only PCA monitor and a fault classifier.

    The PCA model and its threshold are calibrated against ``df_cal``;
    callers should provide independent, normal-operation calibration data.
    Returns ``(scaler, pca, threshold, classifier, X, X_scaled, normal_mean)``
    to support common reconstruction scoring and soft-sensor workflows.
    """
    from sklearn.decomposition import PCA
    from sklearn.ensemble import RandomForestClassifier
    from sklearn.preprocessing import StandardScaler

    X = df_train[list(feature_names)].to_numpy(float)
    faults = df_train["fault"].to_numpy(int)
    normal_mask = faults == 0
    if not normal_mask.any():
        raise ValueError("training data must contain normal samples (fault == 0)")

    scaler = StandardScaler().fit(X[normal_mask])
    X_scaled = scaler.transform(X)
    pca = PCA(n_components=0.90, random_state=7).fit(X_scaled[normal_mask])

    X_cal = scaler.transform(df_cal[list(feature_names)].to_numpy(float))
    reconstruction_error = (
        (X_cal - pca.inverse_transform(pca.transform(X_cal))) ** 2
    ).mean(axis=1)
    threshold = float(np.quantile(reconstruction_error, 0.99))

    classifier = RandomForestClassifier(n_estimators=200, random_state=7, n_jobs=-1)
    classifier.fit(X_scaled, faults)
    normal_mean = X[normal_mask].mean(axis=0)
    return scaler, pca, threshold, classifier, X, X_scaled, normal_mean


def explainSample(bundle, x_row, top_k: int = 5) -> list[tuple[str, float]]:
    """Rank feature deviations from normal by classifier importance.

    The bundle must expose ``clf``, ``scaler``, ``normal_mean`` and
    ``feature_names`` attributes. Contributions are signed standardized
    deviations weighted by each feature's classifier importance.
    """
    if top_k < 1:
        return []
    importance = np.asarray(bundle.clf.feature_importances_, dtype=float)
    scale = np.asarray(bundle.scaler.scale_, dtype=float) + 1e-12
    deviation = (
        np.asarray(x_row, dtype=float) - np.asarray(bundle.normal_mean, dtype=float)
    ) / scale
    contribution = deviation * importance
    indices = np.argsort(-np.abs(contribution))[:top_k]
    return [(bundle.feature_names[i], float(contribution[i])) for i in indices]
