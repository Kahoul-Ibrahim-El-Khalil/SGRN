#!/usr/bin/env python3
"""
sgrn.ml.unified_anomaly — Unified Anomaly Scoring Pipeline (Phase 4).

Composes all three scoring engines behind one interface:

  1. `manifold`   — ProcessManifoldDetector (window autoencoder + per-signal
                    split-conformal bounds + discrete operating context).
                    Physics-informed, primary engine.
  2. `conformal`  — ConformalAnomalyDetector (flat-sample MLP autoencoder +
                    split-conformal threshold from sgrn.ml.anomaly).
                    Secondary engine when no window schema is available.
  3. `classical`  — numpy-only robust z-score fallback (Welford stats +
                    conformal threshold on calibration scores). Works with
                    zero PyTorch installed; also the safety net when the
                    neural engines are uncalibrated or unhealthy.

Fusion strategies:
  - "priority"  — first healthy engine (manifold > conformal > classical)
                  produces the verdict.
  - "ensemble"  — all healthy engines score; verdict = worst (3-state union),
                  score = max normalised exceedance, attribution merged.

The output dict is LLM-doer compatible (sgrn.ai.doer): keys `attribution`,
`feature_attribution`, `suspected_fault`, `anomaly`, `score`, `threshold`,
`top_features` mirror what MockDoer / PlantDoer / LLMDoer consume.

Function naming: camelCase throughout (matches sgrn.ml conventions).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

import numpy as np

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

from .manifold import (
    HAS_TORCH as _MANIFOLD_HAS_TORCH,
    ProcessManifoldDetector,
    _conformalQuantile,
    trainManifoldDetector,
)
from .anomaly import ConformalAnomalyDetector
from .transform import StreamingStats

# ─────────────────────────────────────────────────────────────────────
# Engine health model
# ─────────────────────────────────────────────────────────────────────

_ENGINE_RANK = {"manifold": 3, "conformal": 2, "classical": 1}
_STATE_RANK = {"normal": 0, "uncertain": 1, "abnormal": 2}


def _worstState(stateA: str, stateB: str) -> str:
    return stateA if _STATE_RANK[stateA] >= _STATE_RANK[stateB] else stateB


class UnifiedAnomalyDetector:
    """One detector object, three scoring engines, one verdict.

    Never raises for missing torch: engine availability is probed at
    construction and reflected in :meth:`detectorHealth`. The classical
    engine is always available (numpy only).
    """

    def __init__(
        self,
        featureNames: List[str],
        alpha: float = 0.05,
        fusion: str = "priority",
        manifoldDetector: Optional[ProcessManifoldDetector] = None,
        conformalDetector: Optional[ConformalAnomalyDetector] = None,
        windowSize: int = 12,
    ):
        self.feature_names = list(featureNames)
        self.input_dim = len(featureNames)
        self.alpha = float(alpha)
        if fusion not in ("priority", "ensemble"):
            raise ValueError(f"fusion must be 'priority' or 'ensemble'; got {fusion!r}")
        self.fusion = fusion
        self.window_size = int(windowSize)

        # Engines (optional at construction; attached by fitUnifiedAnomalyDetector)
        self.manifold_detector = manifoldDetector
        self.conformal_detector = conformalDetector

        # Classical fallback stats (Welford, numpy only)
        self.fallback_stats = StreamingStats(dim=self.input_dim)
        # Conformal threshold for the classical score (calibrated classically)
        self.classical_threshold: float = float("inf")
        self.classical_scores_cal: List[float] = []

        # Runtime engine states (refreshed by detectorHealth)
        self._engine_healthy: Dict[str, bool] = {}
        self._engine_reason: Dict[str, str] = {}

    # ── Health probing ──────────────────────────────────────────────

    def detectorHealth(self, refresh: bool = True) -> Dict[str, Dict[str, Any]]:
        """Probe every engine and return {engine: {healthy, reason, rank}}."""
        if refresh:
            self._refreshHealth()
        return {
            engine: {
                "healthy": self._engine_healthy.get(engine, False),
                "reason": self._engine_reason.get(engine, ""),
                "rank": _ENGINE_RANK[engine],
            }
            for engine in ("manifold", "conformal", "classical")
        }

    def _refreshHealth(self) -> None:
        self._engine_healthy = {}
        self._engine_reason = {}

        # manifold: needs torch + calibrated quantile
        if self.manifold_detector is None:
            self._engine_healthy["manifold"] = False
            self._engine_reason["manifold"] = "not configured"
        elif not HAS_TORCH:
            self._engine_healthy["manifold"] = False
            self._engine_reason["manifold"] = "torch unavailable"
        else:
            q = float(self.manifold_detector.q_alpha)
            if not np.isfinite(q):
                self._engine_healthy["manifold"] = False
                self._engine_reason["manifold"] = "uncalibrated (q_alpha=inf)"
            else:
                self._engine_healthy["manifold"] = True
                self._engine_reason["manifold"] = f"q_alpha={q:.4f}"

        # conformal: needs torch + finite threshold
        if self.conformal_detector is None:
            self._engine_healthy["conformal"] = False
            self._engine_reason["conformal"] = "not configured"
        elif not HAS_TORCH:
            self._engine_healthy["conformal"] = False
            self._engine_reason["conformal"] = "torch unavailable"
        elif not np.isfinite(self.conformal_detector.calibrator.quantile_threshold):
            self._engine_healthy["conformal"] = False
            self._engine_reason["conformal"] = "uncalibrated threshold"
        else:
            self._engine_healthy["conformal"] = True
            self._engine_reason["conformal"] = (
                f"threshold={self.conformal_detector.calibrator.quantile_threshold:.4f}")

        # classical: always available once calibrated; uncalibrated still
        # "healthy" but reports inf threshold so callers can see it.
        self._engine_healthy["classical"] = True
        if np.isfinite(self.classical_threshold):
            self._engine_reason["classical"] = f"threshold={self.classical_threshold:.4f}"
        else:
            self._engine_reason["classical"] = "uncalibrated (threshold=inf; pass_calibrate)"

    def healthyEngines(self) -> List[str]:
        """Engines ordered by rank that are currently healthy."""
        return [
            e for e in sorted(
                _ENGINE_RANK, key=lambda k: _ENGINE_RANK[k], reverse=True)
            if self.detectorHealth().get(e, {}).get("healthy", False)
        ]

    # ── Classical engine (numpy-only) ───────────────────────────────

    def calibrateClassicalEngine(self, calibData: np.ndarray) -> float:
        """Calibrate the classical fallback on nominal calibration rows.

        Score = mean squared standardised deviation (norm z-score^2). The
        conformal quantile over these scores gives a threshold with the
        same (approximate, exchangeability-caveated) guarantee as the
        neural engines. Stride-1 autocorrelation caveat applies here too.
        """
        X = np.asarray(calibData, dtype=np.float64)
        if X.ndim == 1:
            X = X.reshape(1, -1)
        if X.shape[1] != self.input_dim:
            raise ValueError(
                f"calibData feature dim {X.shape[1]} != expected {self.input_dim}")

        self.fallback_stats = StreamingStats(dim=self.input_dim)
        # StreamingStats.update expects ONE observation (dim,) per call —
        # feed rows individually so per-coordinate Welford stays exact.
        for row in X:
            self.fallback_stats.update(row)

        stds = self.fallback_stats.std
        z = (X - self.fallback_stats.mean) / stds
        scores = np.mean(z ** 2, axis=1).tolist()
        self.classical_scores_cal = scores
        self.classical_threshold = _conformalQuantile(scores, self.alpha)
        self._refreshHealth()
        return self.classical_threshold

    def scoreClassicalEngine(self, sampleRow: np.ndarray) -> Dict[str, Any]:
        """Score one flat sample row with the classical engine."""
        row = np.asarray(sampleRow, dtype=np.float64).reshape(-1)
        if row.shape[0] != self.input_dim:
            raise ValueError(
                f"sample feature dim {row.shape[0]} != expected {self.input_dim}")

        stds = self.fallback_stats.std
        z = (row - self.fallback_stats.mean) / stds
        per_feature = (z ** 2).tolist()
        score = float(np.mean(z ** 2))
        finite_threshold = self.classical_threshold if np.isfinite(
            self.classical_threshold) else float("inf")
        state = ("normal" if score <= finite_threshold else "abnormal")
        total = sum(per_feature) if sum(per_feature) > 0 else 1.0
        attribution = {
            feat: round(err / total, 4)
            for feat, err in zip(self.feature_names, per_feature)
        }
        return {
            "engine": "classical",
            "score": score,
            "state": state,
            "threshold": finite_threshold,
            "per_feature_scores": per_feature,
            "attribution": attribution,
        }

    # ── Manifold engine (window) ────────────────────────────────────

    def scoreManifoldEngine(
        self, rawWindow: np.ndarray, rawContext: Any = None
    ) -> Dict[str, Any]:
        """Score one raw (window_size, n_features) window with the manifold engine."""
        if self.manifold_detector is None or not HAS_TORCH:
            return {"engine": "manifold", "state": "unavailable", "score": 0.0}
        result = self.manifold_detector.detect(rawWindow, rawContext)
        return {
            "engine": "manifold",
            "score": float(result.get("score", 0.0)),
            "state": result.get("state", "uncertain"),
            "threshold": float(result.get("q_alpha", 0.0)),
            "per_signal_states": result.get("per_signal_states", {}),
            "exceedances": {
                f: result.get("score_max", 0.0)
                for f in result.get("abnormal_signals", [])
            },
            "dimension_breakdown": result.get("dimension_breakdown", {}),
            "top_signals": result.get("top_signals", []),
            "predicted_mode": result.get("predicted_mode", {}),
            "mode_mismatch": result.get("mode_mismatch", []),
            "unseen_mode": bool(result.get("unseen_mode", False)),
        }

    # ── Conformal-MLP engine (flat sample) ─────────────────────────

    def scoreConformalEngine(self, sampleRow: List[float]) -> Dict[str, Any]:
        """Score one flat sample with the ConformalAnomalyDetector engine."""
        if self.conformal_detector is None or not HAS_TORCH:
            return {"engine": "conformal", "state": "unavailable", "score": 0.0}
        is_anomaly, score, attribution = self.conformal_detector.predict(
            list(sampleRow))
        q = self.conformal_detector.calibrator.quantile_threshold
        if not np.isfinite(q):
            state = "uncertain"
        else:
            state = "abnormal" if is_anomaly else "normal"
        return {
            "engine": "conformal",
            "score": float(score),
            "state": state,
            "threshold": float(q),
            "attribution": attribution,
        }

    # ── Unified scoring ─────────────────────────────────────────────

    def scoreWindow(
        self, rawWindow: np.ndarray, rawContext: Any = None,
        flatRow: Optional[np.ndarray] = None,
    ) -> Dict[str, Any]:
        """One unified verdict for a window.

        Args:
            rawWindow: raw (window_size, n_features) engineering-unit window
                (manifold + classical engines). Falls back to `flatRow`
                for the flat engines when the window is missing.
            rawContext: discrete context window (required by manifold engine
                only when it was configured with context features).
            flatRow: last-row flat sample (n_features,) — scored by the
                conformal + classical engines. Defaults to the last row of
                rawWindow when omitted.
        """
        window = np.asarray(rawWindow, dtype=np.float32)
        if flatRow is None:
            flatRow = window[-1] if window.ndim == 2 and window.shape[0] > 0 else window

        engine_results: List[Dict[str, Any]] = []
        health = self.detectorHealth()

        # 1. manifold (highest rank)
        if health["manifold"]["healthy"]:
            engine_results.append(self.scoreManifoldEngine(window, rawContext))

        # 2. conformal
        if health["conformal"]["healthy"]:
            engine_results.append(self.scoreConformalEngine(flatRow.tolist()))

        # 3. classical (always attempted)
        engine_results.append(self.scoreClassicalEngine(np.asarray(flatRow)))

        state = self._fuseState(engine_results)
        score, normaliser = self._fuseScore(engine_results)
        engine_used = self._selectEngine(engine_results)
        attribution = self._fuseAttribution(engine_results, engine_used)

        return {
            "state": state,
            "score": score,
            "engine_used": engine_used,
            "engines_scored": [r["engine"] for r in engine_results],
            "fusion": self.fusion,
            "alpha": self.alpha,
            "attribution": attribution,
            "engine_results": engine_results,
            "window_size": self.window_size,
        }

    def _fuseState(self, engine_results: List[Dict[str, Any]]) -> str:
        """3-state union over available engines, honouring the fusion mode."""
        states = [
            r["state"] for r in engine_results
            if r.get("state") in _STATE_RANK
        ]
        if not states:
            return "uncertain"
        if self.fusion == "ensemble":
            state = states[0]
            for s in states[1:]:
                state = _worstState(state, s)
            return state
        # priority: the highest-ranked healthy engine's own verdict
        # engine_results arrive in descending rank order; take first valid
        for r in engine_results:
            if r.get("state") in _STATE_RANK:
                return r["state"]
        return "uncertain"

    def _fuseScore(
        self, engine_results: List[Dict[str, Any]]
    ) -> Tuple[float, float]:
        """Normalised exceedance (score / threshold, capped at inf handling)."""
        best = 0.0
        for r in engine_results:
            t = r.get("threshold", float("inf"))
            s = float(r.get("score", 0.0))
            if t and np.isfinite(t) and t > 0:
                best = max(best, s / t)
            elif s > 0:
                best = max(best, s)  # uncalibrated: raw score dominates
        return round(float(best), 4), best

    def _selectEngine(self, engine_results: List[Dict[str, Any]]) -> str:
        """Which engine's anatomy drives the verdict/attribution."""
        if self.fusion == "ensemble":
            # pick the engine that fired the largest normalised exceedance
            best_engine, best_val = "classical", -1.0
            for r in engine_results:
                t = r.get("threshold", float("inf"))
                s = float(r.get("score", 0.0))
                v = (s / t) if (t and np.isfinite(t) and t > 0) else s
                # state-weight: abnormal doubles the pull
                if r.get("state") == "abnormal":
                    v *= 2.0
                if v > best_val:
                    best_engine, best_val = r["engine"], v
            return best_engine
        # priority: first engine with a usable state
        for r in engine_results:
            if r.get("state") in _STATE_RANK:
                return r["engine"]
        return "classical"

    def _fuseAttribution(
        self, engine_results: List[Dict[str, Any]], engineUsed: str
    ) -> Dict[str, float]:
        """Per-feature attribution dict compatible with sgrn.ai.doer."""
        chosen = next(
            (r for r in engine_results if r["engine"] == engineUsed), None)
        if chosen is None:
            chosen = engine_results[-1] if engine_results else {}
        attribution = chosen.get("attribution")
        if not attribution:
            attribution = {
                feat: round(1.0 / self.input_dim, 4)
                for feat in self.feature_names
            }
        return attribution

    # ── Sequence scoring ────────────────────────────────────────────

    def scoreSequence(
        self, data: Any,
        features: Optional[List[str]] = None,
        windowSize: Optional[int] = None,
        batchIndex: Optional[np.ndarray] = None,
    ) -> Dict[str, Any]:
        """Score a full timeline.

        Without torch this degrades to the classical engine over rows.
        With torch + a manifold detector, scores engine-major windows via
        manifold.detectBatch when available, else row-wise.

        Returns:
            {"verdicts": List[dict], "summary": {...}} — verdict key schema
            identical to scoreWindow minus engine_results (kept slim for
            bulk streams).
        """
        from .transform import createSlidingWindows, extractFeatureMatrix

        feats = list(features) if features is not None else self.feature_names
        w = int(windowSize) if windowSize is not None else self.window_size

        try:
            feat_matrix = extractFeatureMatrix(data, feats)
        except Exception:
            feat_matrix = np.asarray(data, dtype=np.float32)
            if feat_matrix.ndim == 1:
                feat_matrix = feat_matrix.reshape(-1, 1)

        verdicts: List[Dict[str, Any]] = []
        engine_counts: Dict[str, int] = {"manifold": 0, "conformal": 0, "classical": 0}
        state_counts: Dict[str, int] = {"normal": 0, "uncertain": 0, "abnormal": 0}

        if HAS_TORCH and self.manifold_detector is not None \
                and healthManifold(self) :
            # Fully batched manifold scoring + classical rows
            raw_windows = createSlidingWindows(feat_matrix, windowSize=w, stride=1)
            for i, win in enumerate(raw_windows):
                flat = win[-1] if win.ndim == 2 else win
                res = self.scoreWindow(win, flatRow=np.asarray(flat))
                slim = {k: res[k] for k in
                        ("state", "score", "engine_used", "attribution")}
                verdicts.append(slim)
                engine_counts[res["engine_used"]] = engine_counts.get(
                    res["engine_used"], 0) + 1
                state_counts[res["state"]] = state_counts.get(res["state"], 0) + 1
        else:
            for row in feat_matrix:
                res = self.scoreClassicalEngine(row)
                slim = {
                    "state": res["state"],
                    "score": res["score"],
                    "engine_used": "classical",
                    "attribution": res["attribution"],
                }
                verdicts.append(slim)
                engine_counts["classical"] += 1
                state_counts[res["state"]] = state_counts.get(res["state"], 0) + 1

        n = len(verdicts)
        return {
            "verdicts": verdicts,
            "summary": {
                "count": n,
                "engine_counts": engine_counts,
                "state_counts": state_counts,
                "state_distribution": {
                    s: round(c / n, 4) for s, c in state_counts.items()
                } if n else {},
            },
        }

    # ── LLM-doer compatibility ──────────────────────────────────────

    def buildAnomalyInfo(
        self, windowVerdict: Dict[str, Any], timestamp: Optional[Any] = None
    ) -> Dict[str, Any]:
        """Format a scoreWindow result for sgrn.ai.doer consumers.

        Mirrors the keys PlantDoer / MockDoer / LLMDoer read:
          - attribution / feature_attribution
          - suspected_fault (primary deviating feature)
          - anomaly, score, threshold, top_features, timestamp
        """
        attribution = windowVerdict.get("attribution", {})
        ranked = sorted(attribution.items(), key=lambda kv: kv[1], reverse=True)
        top_features = [name for name, _ in ranked[:3]]
        primary = top_features[0] if top_features else "General"
        exceeds = windowVerdict.get("score", 0.0) > 1.0
        suspected = (
            f"reconstruction deviation on {primary}"
            if exceeds else "baseline consistent (nominal)"
        )
        mr = self.manifold_results(windowVerdict)
        suspected_fault = None
        if mr:
            tops = mr.get("top_signals") or []
            if tops:
                suspected_fault = (
                    f"{tops[0].get('signal', primary)} deviation "
                    f"({tops[0].get('direction', '')})"
                )
        return {
            "anomaly": bool(exceeds),
            "score": float(windowVerdict.get("score", 0.0)),
            "threshold": float(self.manifoldThreshold() or 1.0),
            "alpha": self.alpha,
            "attribution": attribution,
            "feature_attribution": dict(attribution),
            "top_features": top_features,
            "suspected_fault": suspected_fault or suspected,
            "engine_used": windowVerdict.get("engine_used", "classical"),
            "fusion": windowVerdict.get("fusion", self.fusion),
            "window_size": windowVerdict.get("window_size", self.window_size),
            "timestamp": timestamp,
        }

    # ── Small introspection helpers ─────────────────────────────────

    def manifold_results(self, windowVerdict: Dict[str, Any]) -> Dict[str, Any]:
        """Engine-result anatomy for the manifold engine inside a verdict."""
        for r in windowVerdict.get("engine_results", []):
            if r.get("engine") == "manifold":
                return r
        return {}

    def manifoldThreshold(self) -> Optional[float]:
        """Conformal threshold of the highest-ranked calibrated engine."""
        for engine in ("manifold", "conformal"):
            det = (
                self.manifold_detector if engine == "manifold"
                else self.conformal_detector
            )
            if det is None:
                continue
            if engine == "manifold":
                if self.manifold_detector is not None and np.isfinite(
                        float(self.manifold_detector.q_alpha)):
                    return float(self.manifold_detector.q_alpha)
            else:
                q = self.conformal_detector.calibrator.quantile_threshold
                if np.isfinite(q):
                    return float(q)
        if np.isfinite(self.classical_threshold):
            return float(self.classical_threshold)
        return None

    # ── Persistence ─────────────────────────────────────────────────

    def saveUnifiedDetector(self, saveDir: Union[str, Path]) -> Dict[str, Any]:
        """Save all engines. Returns paths dict."""
        save_dir = Path(saveDir)
        save_dir.mkdir(parents=True, exist_ok=True)
        paths: Dict[str, Any] = {}

        if self.manifold_detector is not None and HAS_TORCH:
            try:
                p = self.manifold_detector.save(save_dir)
                paths["manifold"] = str(save_dir)
            except Exception as e:  # manifold.save signatures vary
                paths["manifold_error"] = str(e)

        meta = {
            "alpha": self.alpha,
            "fusion": self.fusion,
            "window_size": self.window_size,
            "feature_names": self.feature_names,
            "classical_threshold": self.classical_threshold,
            "fallback_stats": {
                "mean": self.fallback_stats.mean.tolist(),
                "std": self.fallback_stats.std.tolist(),
                "counts": self.fallback_stats.counts.tolist(),
            },
            "health": self.detectorHealth(refresh=False),
        }
        import json
        meta_path = save_dir / "unified_anomaly_meta.json"
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2)
        paths["meta"] = str(meta_path)
        return paths

    @classmethod
    def loadUnifiedDetector(
        cls, saveDir: Union[str, Path],
        manifoldDetector: Optional[ProcessManifoldDetector] = None,
        conformalDetector: Optional[ConformalAnomalyDetector] = None,
    ) -> "UnifiedAnomalyDetector":
        """Restore from saveUnifiedDetector artifacts.

        The neural engines are heavy objects; callers usually reload them
        via their own save/load (ProcessManifoldDetector.load) and inject
        them here. The classical stats restore from the JSON meta.
        """
        load_dir = Path(saveDir)
        meta_path = load_dir / "unified_anomaly_meta.json"
        if not meta_path.exists():
            raise FileNotFoundError(f"missing {meta_path}")
        import json
        with open(meta_path, "r", encoding="utf-8") as f:
            meta = json.load(f)

        det = cls(
            featureNames=meta.get("feature_names", []),
            alpha=float(meta.get("alpha", 0.05)),
            fusion=meta.get("fusion", "priority"),
            manifoldDetector=manifoldDetector,
            conformalDetector=conformalDetector,
            windowSize=int(meta.get("window_size", 12)),
        )
        det.classical_threshold = float(meta.get("classical_threshold", float("inf")))
        st = meta.get("fallback_stats", {})
        stats = StreamingStats(dim=len(det.feature_names))
        stats.mean = np.asarray(st.get("mean", []), dtype=np.float64)
        st_std = np.asarray(st.get("std", []), dtype=np.float64)
        counts = np.asarray(st.get("counts", []), dtype=np.int64)
        # Rebuild _m2 from restored mean/std/counts (variance*count).
        stats._m2 = np.maximum(st_std, 1e-4) ** 2 * np.maximum(counts, 1)
        stats.counts = counts
        det.fallback_stats = stats
        det._refreshHealth()
        return det


def healthManifold(detector: UnifiedAnomalyDetector) -> bool:
    """Free helper: is the manifold engine usable right now?"""
    if detector.manifold_detector is None or not HAS_TORCH:
        return False
    return np.isfinite(float(detector.manifold_detector.q_alpha))


# ─────────────────────────────────────────────────────────────────────
# High-level fitter (orchestrates training + calibration + fusion)
# ─────────────────────────────────────────────────────────────────────


def fitUnifiedAnomalyDetector(
    data: Any,
    features: Optional[List[str]] = None,
    schemaSource: Optional[Union[str, Path, Dict[str, Any]]] = None,
    alpha: float = 0.05,
    fusion: str = "priority",
    windowSize: int = 12,
    trainManifold: bool = True,
    trainConformal: bool = False,
    conformalEpochs: int = 50,
    device: Optional[Union[str, Any]] = None,
    seed: int = 42,
    **manifoldKwargs: Any,
) -> UnifiedAnomalyDetector:
    """End-to-end fitter for the unified anomaly pipeline.

    Steps:
      1. Resolve features (from schemaSource when provided, else args).
      2. Train + conformal-calibrate the manifold engine via
         trainManifoldDetector (torch only; skipped gracefully).
      3. Optionally train + calibrate the flat conformal-MLP engine.
      4. Calibrate the classical fallback on the same nominal data
         (numpy only — always available).

    torch-less environments return a detector with only the classical
    engine calibrated; scoreWindow then routes to it automatically.
    """
    # ── Feature discovery ───────────────────────────────────────────
    if features is None and schemaSource is not None:
        from .schema import configureModelFromSchema
        schema_cfg = configureModelFromSchema(
            schemaSource=schemaSource, windowSize=windowSize)
        features = list(schema_cfg["features"])
    elif features is None:
        from .pipeline import fitManifoldPipeline  # noqa: F401 (raise path)
        raise ValueError(
            "Provide either 'features' or 'schemaSource' for united fit.")

    # ── Classical calibration data: deterministic first rows ────────
    from .transform import extractFeatureMatrix
    try:
        feat_matrix = extractFeatureMatrix(data, features)
    except Exception:
        feat_matrix = np.asarray(data, dtype=np.float32)
        if feat_matrix.ndim == 1:
            feat_matrix = feat_matrix.reshape(-1, 1)
    n_cal = min(max(int(0.3 * feat_matrix.shape[0]), 16), feat_matrix.shape[0])
    classical_cal = feat_matrix[:n_cal]

    # ── Manifold engine (torch optional) ────────────────────────────
    manifold_detector: Optional[ProcessManifoldDetector] = None
    if trainManifold and HAS_TORCH:
        n = feat_matrix.shape[0]
        if n >= 4 * windowSize:
            n_split = max(int(0.70 * n), windowSize + 2)
            cal_stride = manifoldKwargs.pop("calibrationStride", windowSize)
            from .transform import fitProcessScaler
            scaler = fitProcessScaler(feat_matrix[:n_split])
            try:
                manifold_detector = trainManifoldDetector(
                    X_train=feat_matrix[:n_split],
                    X_cal=feat_matrix[n_split:],
                    scaler=scaler,
                    features=features,
                    config={},
                    seed=seed,
                    device=device,
                    calibrationStride=cal_stride,
                    **manifoldKwargs,
                )
            except Exception:
                # Retry without the optional extras; calibrationStride stays
                # explicit so its value survives this second attempt.
                manifold_detector = trainManifoldDetector(
                    X_train=feat_matrix[:n_split],
                    X_cal=feat_matrix[n_split:],
                    scaler=scaler,
                    features=features,
                    config={},
                    seed=seed,
                    device=device,
                    calibrationStride=cal_stride,
                )
        else:
            # Too few samples for window training — leave manifold detached.
            pass

    # ── Conformal-MLP engine (torch optional) ───────────────────────
    conformal_detector: Optional[ConformalAnomalyDetector] = None
    if trainConformal and HAS_TORCH:
        try:
            conformal_detector = ConformalAnomalyDetector(
                feature_names=features, alpha=alpha)
            flat_train = classical_cal.tolist()
            conformal_detector.fit(flat_train, epochs=conformalEpochs)
            conformal_detector.calibrate(flat_train)
        except Exception:
            conformal_detector = None

    # ── Assemble + calibrate classical ──────────────────────────────
    detector = UnifiedAnomalyDetector(
        featureNames=features,
        alpha=alpha,
        fusion=fusion,
        manifoldDetector=manifold_detector,
        conformalDetector=conformal_detector,
        windowSize=windowSize,
    )
    detector.calibrateClassicalEngine(classical_cal)
    return detector


# ─────────────────────────────────────────────────────────────────────
# Self-test (runs with AND without torch)
# ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import warnings
    warnings.filterwarnings("ignore")

    print("=== unified_anomaly self-test ===")
    print(f"torch: {'available' if HAS_TORCH else 'NOT available (classical only)'}")

    rng = np.random.default_rng(42)
    n_feat = 4
    n_nominal, n_fault = 600, 120

    # Nominal baseline: Gaussian signals ~ N(0, 1)
    nominal = rng.normal(0.0, 1.0, size=(n_nominal, n_feat)).astype(np.float32)

    # Fault: stuck-at sensor drifts strongly on feature 2
    fault = nominal[:n_fault].copy()
    fault[:, 2] += 6.0

    names = ["flow", "pressure", "temp", "level"]

    # 1. Classical-only fit (no torch needed)
    det = fitUnifiedAnomalyDetector(
        data=nominal, features=names, windowSize=12, trainManifold=False)

    health = det.detectorHealth()
    print(f"health: manifold={health['manifold']['healthy']} "
          f"conformal={health['conformal']['healthy']} "
          f"classical={health['classical']['healthy']} "
          f"({health['classical']['reason']})")

    # 2. Score nominal rows — expect majority normal
    nominal_verdicts = [det.scoreWindow(r.reshape(1, -1)) for r in nominal[:200]]
    n_normal = sum(v["state"] == "normal" for v in nominal_verdicts)
    print(f"nominal: {n_normal}/200 normal "
          f"(pass rate {n_normal / 200:.0%}, conformal target >= 95%)")

    # 3. Score fault rows — expect majority abnormal via attribution on 'temp'
    fault_verdicts = [det.scoreWindow(r.reshape(1, -1)) for r in fault]
    n_abnormal = sum(v["state"] == "abnormal" for v in fault_verdicts)
    print(f"fault: {n_abnormal}/{n_fault} abnormal")
    top_feat = max(
        fault_verdicts[0]["attribution"].items(), key=lambda kv: kv[1])[0]
    print(f"top attribution feature: {top_feat} (expected 'temp')")
    assert top_feat == "temp", "attribution must localise the stuck sensor"

    # 4. Ensemble fusion
    det_ens = fitUnifiedAnomalyDetector(
        data=nominal, features=names, windowSize=12,
        trainManifold=False, fusion="ensemble")
    v_ens = det_ens.scoreWindow(fault[5].reshape(1, -1))
    print(f"ensemble verdict: engine={v_ens['engine_used']} state={v_ens['state']}")

    # 5. LLM-doer payload
    info = det.buildAnomalyInfo(nominal_verdicts[0])
    expected_keys = {"anomaly", "score", "threshold", "attribution",
                     "feature_attribution", "top_features", "suspected_fault"}
    missing = expected_keys - set(info.keys())
    assert not missing, f"missing doer keys: {missing}"
    print(f"doer payload OK; suspected_fault={info['suspected_fault']!r}")

    # 6. Sequence scoring + summary
    seq = det.scoreSequence(nominal[:300], features=names, windowSize=12)
    print(f"sequence: {seq['summary']['count']} verdicts, "
          f"states={seq['summary']['state_counts']}")

    # 7. Save / load roundtrip
    paths = det.saveUnifiedDetector("/tmp/opencode/unified_det")
    restored = UnifiedAnomalyDetector.loadUnifiedDetector("/tmp/opencode/unified_det")
    v_orig = det.scoreClassicalEngine(fault[0])
    v_rest = restored.scoreClassicalEngine(fault[0])
    assert abs(v_orig["score"] - v_rest["score"]) < 1e-9, "save/load mismatch"
    print(f"save/load roundtrip OK (threshold "
          f"{paths['meta']} → {restored.classical_threshold:.4f})")

    print("=== all unified_anomaly checks passed ===")
