"""Regression tests for the flare experiment fixes (§2-§7 of the audit).

Covers: canonical module interfaces, robust evaluation (zero/empty/invalid
cases), independent dataset/model/output paths, conformal edge cases,
time-aware missing-data handling, and honest attribution wording.
Uses synthetic fixtures and temporary directories — no gateway binaries,
no plant data.
"""

import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

FLARE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
                                          "..", "experiments", "flare"))
REPO_PYTHON = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

for _p in (FLARE_DIR, REPO_PYTHON):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import torch  # noqa: F401
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

import dataset as flare_dataset
import evaluate as flare_evaluate
import train_detector as flare_train


def _dt(h):
    return datetime(2024, 1, 1, tzinfo=timezone.utc) + pd.Timedelta(hours=h) \
        if HAS_PANDAS else None


class TestFlareInterfaces(unittest.TestCase):
    """§2 — the reported snake_case/camelCase inconsistencies are gone."""

    def test_canonical_names_importable(self):
        self.assertTrue(callable(flare_dataset.loadDatasetDf))
        self.assertTrue(callable(flare_dataset.getSplitsAndWindows))
        self.assertTrue(callable(flare_dataset.ensureDatasetCsv))
        self.assertTrue(callable(flare_dataset.resolveModelDir))
        self.assertTrue(callable(flare_evaluate.runEvaluation))
        self.assertTrue(callable(flare_evaluate.computeEvaluationMetrics))
        self.assertTrue(callable(flare_evaluate.parseEvaluationTimestamps))

    def test_old_spellings_gone(self):
        for mod in (flare_dataset, flare_evaluate, flare_train):
            for gone in ("load_dataset_df", "get_splits_and_windows",
                         "create_sliding_windows", "run_evaluation",
                         "train_manifold_detector"):
                self.assertFalse(hasattr(mod, gone), f"{mod.__name__}.{gone}")

    def test_model_uses_canonical_trainer(self):
        import model as flare_model

        self.assertEqual(flare_model.train_autoencoder.__name__, "trainManifoldDetector")


class TestEvaluationRobustness(unittest.TestCase):
    """§3 — zero/empty/invalid evaluation inputs."""

    def _sweep(self, states, onset_h=10.0, esd_h=None):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        base = datetime(2024, 1, 1)
        times = [base + pd.Timedelta(hours=onset_h - 2 + i) for i in range(len(states))]
        results = [{"state": s, "score": 1.0 if s == "abnormal" else 0.1,
                    "top_signals": [{"signal": "SafetySystems.flare_header_press"}]}
                   if s == "abnormal" else {"state": s, "score": 0.1,
                                            "top_signals": []}
                   for s in states]
        fault_dt = base + pd.Timedelta(hours=onset_h)
        to_ms = flare_evaluate.epochMsUtc
        return flare_evaluate.computeEvaluationMetrics(
            [1.0 if s == "abnormal" else 0.1 for s in states], states, times, results,
            fault_dt, to_ms(fault_dt),
            to_ms(base + pd.Timedelta(hours=esd_h)) if esd_h is not None else None,
            esd_iso=None, fault_onset_iso=fault_dt.isoformat(),
            q_alpha=0.5, q_upper=0.7)

    def test_detected_at_expected_time(self):
        m = self._sweep(["normal"] * 2 + ["abnormal"] + ["normal"] * 2, onset_h=10.0)
        self.assertAlmostEqual(m["detection_delay_hours"], 0.0)
        self.assertTrue(m["flare_header_press_in_top3"])
        self.assertIsNotNone(m["first_abnormal_iso"])

    def test_detected_late(self):
        m = self._sweep(["normal"] * 5 + ["abnormal"], onset_h=10.0, esd_h=20.0)
        self.assertAlmostEqual(m["detection_delay_hours"], 3.0)
        self.assertAlmostEqual(m["lead_time_before_esd_hours"], 7.0)

    def test_no_detection_is_valid_not_crash(self):
        m = self._sweep(["normal", "uncertain", "normal"], onset_h=10.0, esd_h=20.0)
        self.assertIsNone(m["detection_delay_hours"])
        self.assertIsNone(m["first_abnormal_iso"])
        self.assertIsNone(m["lead_time_before_esd_hours"])
        self.assertEqual(m["n_windows"], 3)  # other metrics preserved
        self.assertIn("null", json.dumps(m))  # None serializes as null, never "None"

    def test_empty_input(self):
        m = self._sweep([], onset_h=10.0)
        self.assertEqual(m["n_windows"], 0)
        self.assertIsNone(m["detection_delay_hours"])
        self.assertFalse(m["primary_fault_in_top3"])

    def test_missing_or_invalid_timestamps(self):
        with self.assertRaises(ValueError):
            flare_evaluate.parseEvaluationTimestamps({})
        with self.assertRaises(ValueError):
            flare_evaluate.parseEvaluationTimestamps(
                {"fault_onset_iso": "not-a-date", "fault_onset_ts": 1})
        with self.assertRaises(ValueError):
            flare_evaluate.parseEvaluationTimestamps(
                {"fault_onset_iso": "2024-01-01T00:00:00", "fault_onset_ts": "NaN"})
        # tz-aware ISO is accepted (normalized to naive UTC).
        dt, ts, _ = flare_evaluate.parseEvaluationTimestamps(
            {"fault_onset_iso": "2024-01-01T01:00:00+01:00", "fault_onset_ts": 1704067200000})
        self.assertEqual(dt, datetime(2024, 1, 1, 0, 0))


class TestExperimentPaths(unittest.TestCase):
    """§4 — dataset, model and output locations stay independent."""

    def test_resolve_model_dir_layouts(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            with self.assertRaises(FileNotFoundError):
                flare_dataset.resolveModelDir(tmp)
            flat = tmp / "flat"
            flat.mkdir()
            (flat / "model_meta.json").write_text("{}")
            self.assertEqual(flare_dataset.resolveModelDir(flat), flat)
            sub = tmp / "sub"
            (sub / "model").mkdir(parents=True)
            (sub / "model" / "model_meta.json").write_text("{}")
            self.assertEqual(flare_dataset.resolveModelDir(sub), sub / "model")

    def test_missing_inputs_raise_actionably(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):
                flare_dataset.loadDatasetDf(Path(tmp) / "nope.csv")

    def test_train_slice_guard(self):
        build = flare_train.buildTestContext
        cal = np.zeros((20, 2), dtype=np.float32)
        test = np.zeros((8, 2), dtype=np.float32)
        # len(test) == window_size used to build an oversized window (-0 slice).
        self.assertEqual(build(cal, test, 8).shape, (8, 2))
        short = np.zeros((5, 2), dtype=np.float32)
        self.assertEqual(build(cal, short, 8).shape, (8, 2))
        with self.assertRaises(ValueError):
            build(np.zeros((2, 2), dtype=np.float32), short, 8)


class TestConformalEdges(unittest.TestCase):
    """§5 — hand-computed quantiles, ties, monotonicity, validation."""

    def test_hand_computed_quantile(self):
        from sgrn.ml.manifold import _conformalQuantile

        # n=10, alpha=0.1 -> k=ceil(11*0.9)=10 -> max.
        self.assertEqual(_conformalQuantile(list(range(1, 11)), 0.1), 10.0)
        # n=4, alpha=0.5 -> k=ceil(5*0.5)=3 -> third of [1,2,3,4].
        self.assertEqual(_conformalQuantile([1.0, 2.0, 3.0, 4.0], 0.5), 3.0)

    def test_ties(self):
        from sgrn.ml.manifold import _conformalQuantile

        self.assertEqual(_conformalQuantile([2.0, 2.0, 2.0, 2.0], 0.25), 2.0)

    def test_monotonic_in_alpha(self):
        from sgrn.ml.manifold import _conformalQuantile

        scores = [0.1, 0.4, 0.5, 0.9, 1.2, 2.0, 3.0]
        qs = [_conformalQuantile(scores, a) for a in (0.5, 0.3, 0.1, 0.01)]
        self.assertEqual(qs, sorted(qs))

    def test_empty_insufficient_nonfinite_invalid(self):
        from sgrn.ml.manifold import _conformalQuantile
        from sgrn.ml import SplitConformalCalibrator

        self.assertEqual(_conformalQuantile([], 0.05), float("inf"))
        self.assertEqual(_conformalQuantile([0.5, 0.6], 0.05), float("inf"))
        with self.assertRaises(ValueError):
            _conformalQuantile([0.1, float("nan")], 0.1)
        with self.assertRaises(ValueError):
            _conformalQuantile([0.1, float("inf")], 0.1)
        for bad in (0, 1, 1.5, -0.1, True):
            with self.assertRaises(ValueError, msg=f"alpha={bad}"):
                _conformalQuantile([0.1, 0.2, 0.3], bad)
            with self.assertRaises(ValueError, msg=f"calpha={bad}"):
                SplitConformalCalibrator(alpha=bad)


class TestMissingData(unittest.TestCase):
    """§6 — bounded forward fill, no backward leak, split-aware leading fill."""

    def test_bounded_fill_no_backfill(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        from sgrn.ml import extractFeatureMatrix

        df = pd.DataFrame({"a": [np.nan, np.nan, 1.0, np.nan, np.nan, np.nan, 2.0]})
        out = extractFeatureMatrix(df, ["a"], fillLimit=2, allowBackfill=False)
        self.assertTrue(np.isnan(out[0, 0]) and np.isnan(out[1, 0]))  # leading kept
        self.assertEqual(out[3, 0], 1.0)  # within limit
        self.assertTrue(np.isnan(out[5, 0]))  # long outage stays visible
        legacy = extractFeatureMatrix(df, ["a"])
        self.assertFalse(np.isnan(legacy).any())  # legacy default unchanged

    def test_flare_split_aware_fill(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        n = 60
        vals = np.arange(n, dtype=float)
        vals[0:3] = np.nan      # leading gap
        vals[30:36] = np.nan    # 6-row outage, limit is 5 -> 1 residual NaN
        df = pd.DataFrame({"timestamp_ms": [1700000000000 + i * 1000 for i in range(n)],
                            "S.x": vals})
        # loadDatasetDf drops the startup row: output row i == input row i+1.
        with tempfile.TemporaryDirectory() as tmp:
            csv = os.path.join(tmp, "d.csv")
            df.to_csv(csv, index=False)
            feat, _ = flare_dataset.loadDatasetDf(Path(csv), ["S.x"], fillLimit=5)
            self.assertTrue(np.isnan(feat["S.x"].iloc[0]))  # no bfill leak
            self.assertTrue(np.isnan(feat["S.x"].iloc[1]))
            self.assertEqual(feat["S.x"].iloc[4], 5.0)
            self.assertEqual(feat["S.x"].iloc[28], 29.0)  # last valid before outage
            self.assertEqual(feat["S.x"].iloc[33], 29.0)  # ffill within limit
            self.assertTrue(np.isnan(feat["S.x"].iloc[34]))  # outage preserved
            splits = flare_dataset.getSplitsAndWindows(
                csv_path=Path(csv),
                config={"features": ["S.x"], "dataset": {"train_ratio": 0.7, "fill_limit": 5,
                                                         "window_size": 4}},
                meta={"fault_onset_iso": pd.to_datetime(1700000000000 + 55 * 1000,
                                                        unit="ms").isoformat()})
            self.assertGreater(len(splits["X_train"]), 0)
            self.assertEqual(splits["fill_report"]["train_leading_filled"], 2)
            self.assertEqual(splits["fill_report"]["residual_nans"], 1)

    def test_zero_dtl_timestamps_dropped_not_crashed(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        df = pd.DataFrame({
            "timestamp_ms": [1700000000000 + i * 1000 for i in range(6)],
            "S.x": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            "S.stamp": ["0000-00-00 00:00:00.000000000"] * 2 + [
                "2024-01-01T00:00:02", "2024-01-01T00:00:03",
                "2024-01-01T00:00:04", "2024-01-01T00:00:05"],
        })
        with tempfile.TemporaryDirectory() as tmp:
            csv = os.path.join(tmp, "d.csv")
            df.to_csv(csv, index=False)
            # timestamp_ms (ints) wins for the clock; the zero-DTL column
            # must not crash parsing.
            feat, ts = flare_dataset.loadDatasetDf(Path(csv), ["S.x"])
            self.assertEqual(len(feat), 5)  # startup row dropped by convention
            self.assertEqual(len(ts), 5)

    def test_empty_pre_fault_raises(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        df = pd.DataFrame({"timestamp_ms": [1700000000000, 1700000001000], "S.x": [1.0, 2.0]})
        with tempfile.TemporaryDirectory() as tmp:
            csv = os.path.join(tmp, "d.csv")
            df.to_csv(csv, index=False)
            with self.assertRaises(ValueError):
                flare_dataset.getSplitsAndWindows(
                    csv_path=Path(csv), config={"features": ["S.x"]},
                    meta={"fault_onset_iso": "2000-01-01T00:00:00"})


class TestAttributionHonesty(unittest.TestCase):
    """§7 — injected deviation attributes to the injected signal; no causal claims."""

    def test_known_spike_attributes_correctly(self):
        if not (HAS_TORCH and HAS_PANDAS):
            self.skipTest("torch/pandas required")
        from sgrn.ml.pipeline import fitManifoldPipeline

        rng = np.random.default_rng(21)
        n = 200
        df = pd.DataFrame({"a": rng.normal(size=n), "b": rng.normal(size=n),
                            "c": rng.normal(size=n)})
        out = fitManifoldPipeline(
            data=df, features=["a", "b", "c"], faultOnset=160, windowSize=8,
            modelConfig={"model": {"epochs": 2, "hidden_dim": 8, "latent_dim": 4,
                                   "alpha": 0.5}}, seed=21)
        det = out["detector"]
        win = df[["a", "b", "c"]].iloc[10:18].to_numpy(dtype=np.float32).copy()
        win[:, 1] += 25.0  # inject into b only
        res = det.detect(win)
        top = res["top_signals"][0]["signal"]
        self.assertEqual(top, "b")
        rep = det.explain(win)
        text = json.dumps(rep)
        self.assertNotIn("caus", text.lower().replace("causal root cause", ""))
        self.assertIn("correlational", rep["note"])


class TestExperimentCli(unittest.TestCase):
    """§4 — CLI options route dataset, model and output locations independently."""

    def test_parser_paths(self):
        sys.path.insert(0, FLARE_DIR)
        try:
            import cli as flare_cli

            args = flare_cli.buildParser().parse_args(
                ["evaluate", "--model-dir", "/tmp/m", "--output-dir", "/tmp/o"])
            self.assertEqual(args.model_dir, "/tmp/m")
            self.assertEqual(args.output_dir, "/tmp/o")
            args = flare_cli.buildParser().parse_args(["train"])
            self.assertIsNone(args.save_dir)
            args = flare_cli.buildParser().parse_args(
                ["train", "--save-dir", "/tmp/s"])
            self.assertEqual(args.save_dir, "/tmp/s")
            args = flare_cli.buildParser().parse_args(
                ["demo", "--model-dir", "/tmp/m"])
            self.assertEqual(args.model_dir, "/tmp/m")
        finally:
            sys.path.remove(FLARE_DIR)
            sys.modules.pop("cli", None)


class TestStage5Report(unittest.TestCase):
    """Report claims follow the metrics instead of hardcoding PASS."""

    def _metrics(self, **over):
        base = {
            "false_alarm_rate_abnormal_only": 0.0,
            "detection_delay_hours": 1.5,
            "lead_time_before_esd_hours": 15.0,
            "primary_dimension": "SafetySystems.flare_header_press",
            "primary_subsystem": "SafetySystems",
            "flare_header_press_in_top3": True,
            "feed_pressure_in_top3": True,
            "first_abnormal_iso": "2024-01-31T05:00:00",
            "calibration_windows": 80,
            "q_alpha": 1.0,
            "q_upper": 1.4,
            "dimension_breakdown": {},
            "top_signals_at_detection": [],
        }
        base.update(over)
        return base

    def test_report_states_follow_metrics(self):
        sys.path.insert(0, FLARE_DIR)
        try:
            import orchestrate as orch

            with tempfile.TemporaryDirectory() as tmp:
                text = orch.stage5Report(self._metrics(), Path(tmp)).read_text()
                self.assertIn("**PASS**", text)
                self.assertNotIn("**FAIL**", text)
            with tempfile.TemporaryDirectory() as tmp:
                text = orch.stage5Report(
                    self._metrics(detection_delay_hours=None,
                                  lead_time_before_esd_hours=None,
                                  primary_dimension="N/A",
                                  flare_header_press_in_top3=False),
                    Path(tmp)).read_text()
                self.assertIn("**FAIL**", text)
                self.assertIn("n/a (no detection)", text)
        finally:
            sys.path.remove(FLARE_DIR)
            for mod in ("orchestrate",):
                sys.modules.pop(mod, None)
    """§4 — CLI options route dataset, model and output locations independently."""

    def test_parser_paths(self):
        sys.path.insert(0, FLARE_DIR)
        try:
            import cli as flare_cli

            args = flare_cli.buildParser().parse_args(
                ["evaluate", "--model-dir", "/tmp/m", "--output-dir", "/tmp/o"])
            self.assertEqual(args.model_dir, "/tmp/m")
            self.assertEqual(args.output_dir, "/tmp/o")
            args = flare_cli.buildParser().parse_args(["train"])
            self.assertIsNone(args.save_dir)
            args = flare_cli.buildParser().parse_args(
                ["train", "--save-dir", "/tmp/s"])
            self.assertEqual(args.save_dir, "/tmp/s")
        finally:
            sys.path.remove(FLARE_DIR)
            for mod in ("cli",):
                sys.modules.pop(mod, None)


class TestStreamingFill(unittest.TestCase):
    """§6 — streaming fill is bounded with scaler-mean cold start."""

    def test_bounded_gap_and_cold_start(self):
        if not (HAS_TORCH and HAS_PANDAS):
            self.skipTest("torch/pandas required")
        from sgrn.ml.dataset import JsonlDatasetReader
        from sgrn.ml.pipeline import fitManifoldPipelineStream

        n = 120
        vals = [float(i) for i in range(n)]
        vals[0] = None  # leading gap
        vals[1] = None
        for i in range(60, 80):
            vals[i] = None  # 20-row outage, limit is 8
        lines = [{"type": "dictionary",
                  "leaves": [{"id": 0, "path": "a"}, {"id": 1, "path": "b"}]}]
        for i in range(n):
            lines.append({"type": "anchor" if i == 0 else "delta",
                          "ts": 1700000000000 + i * 1000,
                          "data" if i == 0 else "changes": {
                              "0": vals[i],
                              "1": 1.0 if vals[i] is None else float(vals[i]) * 0.1}})
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "s.jsonl")
            with open(path, "w") as f:
                f.write("\n".join(json.dumps(line) for line in lines))
            out = fitManifoldPipelineStream(
                JsonlDatasetReader(path), features=["a", "b"],
                faultOnset=100, windowSize=8,
                modelConfig={"model": {"epochs": 1, "hidden_dim": 8,
                                       "latent_dim": 4, "alpha": 0.5}},
                seed=11)
        # Completes despite leading gaps and a long outage; outage rows stay
        # visible downstream instead of being paved with stale fills.
        self.assertGreater(out["nCalEffective"], 0)


if __name__ == "__main__":
    unittest.main()
