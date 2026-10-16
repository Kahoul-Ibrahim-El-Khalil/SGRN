"""Regression tests for the SGRN ML subsystem audit (§13).

Synthetic fixtures only — nothing here validates against a real plant.
Covers: #UNIT/metadata survival, detector fit/calibrate/predict/explain,
small-calibration behaviour, chronological splits and leakage, malformed
input, artifact compatibility, train/infer consistency, optional
dependencies, naming/API compatibility, and streaming/archive paths.
"""

import json
import os
import sys
import tempfile
import unittest
import warnings

repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(repo_root, "sgrn", "python"))

import numpy as np

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

try:
    import zstandard
    HAS_ZSTD = True
except ImportError:
    HAS_ZSTD = False

from sgrn.ml import (
    ConformalAnomalyDetector,
    SplitConformalCalibrator,
    StreamingStats,
    configureModelFromSchema,
    extractFeatureMatrix,
    fitProcessScaler,
    parseSclSchema,
    scoreDetections,
    signalsFromRegistry,
    splitProcessTimeline,
)
from sgrn.ml import diagnostics as _diagnostics
from sgrn.ml import manifold as _manifold
from sgrn.ml import transform as _transform
from sgrn.dtypes import unpackDtl


REGISTRY = {
    "dbs": [
        {
            "db_number": 1,
            "db_name": "Skid",
            "size_bytes": 32,
            "endianness": "little",
            "fields": [
                {"name": "pressure", "offset": 0, "bit": 0, "type": "Real",
                 "unit": "bar", "dimension": "Pressure", "min": 0.0, "max": 10.0,
                 "description": "line pressure"},
                {"name": "running", "offset": 4, "bit": 0, "type": "Bool"},
                {"name": "mode", "offset": 5, "bit": 0, "type": "USInt",
                 "enum": {"0": "STOPPED", "1": "RUNNING"}},
                {"name": "stamp", "offset": 8, "bit": 0, "type": "DTL"},
                {"name": "scratch", "offset": 20, "bit": 0, "type": "Real",
                 "is_transient": True},
            ],
        }
    ]
}

SCL_TEXT = """
DATA_BLOCK "Skid" DB 1 #LITTLE_ENDIAN
VAR
    pressure : Real #UNIT("bar") #RANGE(0.0, 10.0);
    running  : Bool;
    mode     : USInt #ENUM(STOPPED, RUNNING);
    stamp    : DTL;
END_VAR
END_DATA_BLOCK
"""


def _tiny_frame(n=120, seed=3):
    rng = np.random.default_rng(seed)
    heat = np.array([True] * (n // 2) + [False] * (n - n // 2))
    return {
        "temp": np.where(heat, 50.0 + rng.normal(size=n), 10.0 + rng.normal(size=n)),
        "flow": rng.normal(size=n) + 5.0,
        "heater_on": heat,
        "ts": [1700000000000 + i * 1000 for i in range(n)],
    }


class TestSchemaMetadata(unittest.TestCase):
    def test_registry_units_dimensions_ranges_survive(self):
        sigs = signalsFromRegistry(REGISTRY)["signals"]
        self.assertEqual(sigs["Skid.pressure"]["unit"], "bar")
        self.assertEqual(sigs["Skid.pressure"]["dimension"], "Pressure")
        self.assertEqual(sigs["Skid.pressure"]["min_value"], 0.0)
        self.assertEqual(sigs["Skid.pressure"]["max_value"], 10.0)
        self.assertEqual(sigs["Skid.pressure"]["comment"], "line pressure")
        self.assertEqual(sigs["Skid.mode"]["enum"], ["STOPPED", "RUNNING"])
        self.assertNotIn("Skid.scratch", sigs)  # transients are not telemetry

    def test_scl_fallback_agrees_on_names(self):
        scl = parseSclSchema(SCL_TEXT)
        reg = signalsFromRegistry(REGISTRY)
        self.assertEqual(set(scl["signals"]), set(reg["signals"]))

    def test_configure_splits_roles(self):
        cfg = configureModelFromSchema(REGISTRY)
        self.assertIn("Skid.pressure", cfg["features"])
        self.assertIn("Skid.running", cfg["context_features"])
        self.assertIn("Skid.mode", cfg["context_features"])
        self.assertIn("Skid.stamp", cfg["timestamp_fields"])
        self.assertNotIn("Skid.stamp", cfg["features"])
        self.assertEqual(cfg["featureDimensions"]["Skid.pressure"], "Pressure")

    def test_dtl_decodes_to_ns(self):
        import calendar
        import struct

        raw = struct.pack("<H", 2024) + bytes([3, 18, 1, 19, 30, 5]) + struct.pack("<I", 123456789)
        expect = calendar.timegm((2024, 3, 18, 19, 30, 5)) * 1_000_000_000 + 123456789
        self.assertEqual(unpackDtl(raw, t_little_endian=True), expect)
        with self.assertRaises(Exception):
            unpackDtl(b"\x00" * 11)


class TestPartitioning(unittest.TestCase):
    def test_chronological_split_and_no_leakage(self):
        data = np.arange(100, dtype=np.float32).reshape(100, 1)
        splits = splitProcessTimeline(data, trainRatio=0.7, calRatio=0.3, faultOnsetIdx=80)
        self.assertEqual(len(splits["trainRaw"]) + len(splits["calRaw"]), 80)
        self.assertEqual(len(splits["testRaw"]), 20)
        # Train region holds rows [0, 56); nothing at/after the fault onset.
        self.assertTrue(np.all(splits["trainRaw"][:, 0] < 80))
        self.assertTrue(np.all(splits["calRaw"][:, 0] < 80))
        self.assertEqual(splits["trainRaw"][0, 0], 0.0)  # chronological, not shuffled

    def test_numpy_integer_onset(self):
        data = np.zeros((50, 2), dtype=np.float32)
        splits = splitProcessTimeline(data, faultOnsetIdx=np.int64(40))
        self.assertEqual(len(splits["testRaw"]), 10)

    def test_missing_malformed_reordered(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        df = pd.DataFrame({"b": [1.0, 2.0], "a": [3.0, 4.0]})
        out = extractFeatureMatrix(df, ["a", "b"])  # reordered columns follow names
        self.assertEqual(out.tolist(), [[3.0, 1.0], [4.0, 2.0]])
        with self.assertRaises(KeyError):
            extractFeatureMatrix(df, ["a", "missing"])
        with self.assertRaises((ValueError, TypeError)):
            extractFeatureMatrix(np.zeros((2, 3), dtype=np.float32), ["a", "b"])


class TestConformal(unittest.TestCase):
    def test_small_calibration_never_flags(self):
        cal = SplitConformalCalibrator(alpha=0.05)
        self.assertEqual(cal.calibrate([0.5, 0.6]), float("inf"))
        self.assertEqual(cal.calibrate([]), float("inf"))

    def test_detector_interface_and_explain(self):
        det = ConformalAnomalyDetector(["a", "b"], alpha=0.5)
        det.fit([[0.0, 0.0]] * 30, epochs=2)
        det.calibrate([[0.0, 0.0]] * 30)
        flag, score, attr = det.predict([0.0, 0.0])
        self.assertEqual(flag, score > det.calibrator.quantile_threshold)
        rep = det.explain([0.0, 0.0])
        self.assertIn("threshold", rep)
        self.assertIn("top_features", rep)
        self.assertIn("causal", rep["note"])

    def test_removed_spellings_are_gone(self):
        det = ConformalAnomalyDetector(["a"], alpha=0.5)
        for gone in ("train_baseline", "calibrate_conformal", "evaluate_sample"):
            self.assertFalse(hasattr(det, gone), gone)
            self.assertFalse(hasattr(det.calibrator, gone), gone)
        # Canonical interface works with no warnings involved.
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            det.fit([[0.0]] * 10, epochs=1)
            det.calibrate([[0.0]] * 10)
            det.predict([0.0])
            det.explain([0.0])
            det.calibrator.evaluateSample([0.0], [0.0])

    def test_torchless_predict_still_answers(self):
        det = ConformalAnomalyDetector(["a", "b"], alpha=0.5)
        det.model = None  # simulate missing torch backend
        flag, score, attr = det.predict([3.0, 4.0])
        self.assertEqual(score, 5.0)
        self.assertIn("a", attr)


class TestMetrics(unittest.TestCase):
    def test_score_detections_hand_computed(self):
        m = scoreDetections(["abnormal", "normal", "uncertain", "abnormal"],
                            [1, 0, 1, 0])
        self.assertEqual((m["true_positives"], m["false_positives"],
                          m["true_negatives"], m["false_negatives"]), (1, 1, 1, 1))
        self.assertEqual(m["uncertain"], 1)
        self.assertAlmostEqual(m["precision"], 0.5)
        with self.assertRaises(ValueError):
            scoreDetections(["normal"], [0, 1])

    def test_score_detections_empty_is_nan_not_perfect(self):
        m = scoreDetections([], [])
        self.assertTrue(np.isnan(m["precision"]))


class TestManifoldBundle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HAS_TORCH or not HAS_PANDAS:
            raise unittest.SkipTest("torch/pandas required")
        from sgrn.ml.pipeline import fitManifoldPipeline

        d = _tiny_frame()
        df = pd.DataFrame({**{k: v for k, v in d.items() if k != "ts"},
                           "ts": pd.to_datetime(d["ts"], unit="ms")})
        cls.out = fitManifoldPipeline(
            data=df, features=["temp", "flow"], contextFeatures=["heater_on"],
            timestampCol="ts", faultOnset=90, windowSize=8,
            modelConfig={"model": {"epochs": 2, "hidden_dim": 8, "latent_dim": 4,
                                   "alpha": 0.5}},
            modeHeadConfig={"epochs": 2, "hidden_dim": 8}, seed=11)
        cls.det = cls.out["detector"]
        cls.df = df

    def test_feature_identity_and_units_propagate(self):
        self.assertEqual(self.det.features, ["temp", "flow"])
        # No schema source here: name-based fallback still maps every feature.
        self.assertEqual(set(self.det.feature_dimensions), {"temp", "flow"})
        self.assertIsNone(self.out["featureDimensions"])
        rep = self.det.explain(
            self.df[["temp", "flow"]].iloc[10:18].to_numpy(dtype=np.float32),
            self.df[["heater_on"]].iloc[10:18])
        self.assertIn("unit_dimension", rep["suspect_signals"][0])
        self.assertIn("correlational", rep["note"])

    def test_predict_alias_and_batch_consistency(self):
        w = self.df[["temp", "flow"]].iloc[10:18].to_numpy(dtype=np.float32)
        c = self.df[["heater_on"]].iloc[10:18]
        single = self.det.detect(w, c)
        self.assertEqual(self.det.predict(w, c)["state"], single["state"])
        # raw (object) context windows for the batch path:
        raw_ctx = np.empty((1, 8, 1), dtype=object)
        raw_ctx[0, :, 0] = np.asarray(c).reshape(-1)
        batch = self.det.detectBatch(w[np.newaxis, :, :], raw_ctx, batchSize=8)
        self.assertAlmostEqual(batch["scores"][0], single["score"], places=5)
        self.assertEqual(batch["states"][0], single["state"])

    def test_artifact_rejects_incompatible_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.det.save(tmp, trainConfig=self.out["trainConfig"])
            import json as _json

            meta = _json.load(open(os.path.join(tmp, "model_meta.json")))
            self.assertIn("sgrn_version", meta)
            self.assertIn("train_config", meta)
            self.assertEqual(meta["train_config"]["seed"], 11)
            from sgrn.ml.manifold import ProcessManifoldDetector

            ok = ProcessManifoldDetector.load(tmp, expectedFeatures=["temp", "flow"])
            self.assertEqual(ok.features, ["temp", "flow"])
            with self.assertRaises(ValueError):
                ProcessManifoldDetector.load(tmp, expectedFeatures=["temp", "WRONG"])
            meta["scaler"]["feature_names"] = ["flow", "temp"]
            _json.dump(meta, open(os.path.join(tmp, "model_meta.json"), "w"))
            with self.assertRaises(ValueError):
                ProcessManifoldDetector.load(tmp)

    def test_registry_units_reach_explain(self):
        if not HAS_PANDAS:
            self.skipTest("pandas not installed")
        from sgrn.ml.pipeline import fitManifoldPipeline

        rng = np.random.default_rng(9)
        n = 120
        df = pd.DataFrame({
            "Skid.pressure": rng.normal(5.0, 0.5, size=n),
            "Skid.running": rng.random(n) < 0.8,
            "Skid.mode": rng.integers(0, 2, size=n),
        })
        out = fitManifoldPipeline(
            data=df, schemaSource=REGISTRY, faultOnset=100, windowSize=8,
            modelConfig={"model": {"epochs": 1, "hidden_dim": 8, "latent_dim": 4,
                                   "alpha": 0.5}},
            modeHeadConfig={"epochs": 1, "hidden_dim": 8}, seed=5)
        det = out["detector"]
        self.assertEqual(det.features, ["Skid.pressure"])
        self.assertEqual(det.feature_dimensions["Skid.pressure"], "Pressure")
        w = df[["Skid.pressure"]].iloc[10:18].to_numpy(dtype=np.float32)
        rep = det.explain(w, df[["Skid.running", "Skid.mode"]].iloc[10:18])
        sig = rep["suspect_signals"][0]
        self.assertEqual(sig["signal"], "Skid.pressure")
        self.assertEqual(sig["unit_dimension"], "Pressure")
        # Engineering units, not standardized space, in the report.
        self.assertAlmostEqual(sig["current_value"], float(df["Skid.pressure"][17]),
                               places=5)

    def test_train_infer_preprocessing_consistency(self):
        from sgrn.ml.pipeline import inferStateAt, predictFeature

        r = inferStateAt(self.det, self.df, self.df["ts"][50], timestampCol="ts",
                         contextData=self.df[["heater_on"]])
        self.assertIn(r["state"], ("normal", "uncertain", "abnormal"))
        p = predictFeature(self.det, self.df[["temp", "flow"]].iloc[50:58], "temp",
                           context=self.df[["heater_on"]].iloc[50:58])
        self.assertIn("interval", p)
        self.assertIn("surprise", p)


class TestNaming(unittest.TestCase):
    def test_canonical_names_and_compat_aliases(self):
        import sgrn.ml as ml

        for name in ["fitManifoldPipeline", "trainManifoldDetector",
                     "extractFeatureMatrix", "createSlidingWindows",
                     "configureModelFromSchema", "StreamingStats",
                     "fitManifoldPipelineStream", "inferStateAt", "predictFeature",
                     "scoreDetections", "signalsFromRegistry"]:
            self.assertTrue(hasattr(ml, name), name)
        self.assertIs(ml.train_manifold_detector, ml.trainManifoldDetector)
        self.assertIs(ml.fit_manifold_pipeline, ml.fitManifoldPipeline)
        self.assertIs(ml.extract_feature_matrix, ml.extractFeatureMatrix)
        self.assertIs(ml.parse_scl_schema, ml.parseSclSchema)
        # Params keep their published camelCase spelling (API stability);
        # new code uses snake_case locals. Spot-check the contract:
        import inspect

        self.assertIn("windowSize", inspect.signature(ml.fitManifoldPipeline).parameters)
        # Classes are PascalCase, constants UPPER_SNAKE_CASE.
        self.assertTrue(ml.ProcessManifoldDetector.__name__[0].isupper())
        self.assertTrue(ml.SplitConformalCalibrator.__name__[0].isupper())


class TestStreaming(unittest.TestCase):
    def test_welford_matches_batch(self):
        rng = np.random.default_rng(0)
        x = rng.normal(size=(2000, 4))
        ref = fitProcessScaler(x, ["a", "b", "c", "d"])
        s = StreamingStats(4)
        for row in x:
            s.update(row)
        got = s.toScaler(["a", "b", "c", "d"])
        self.assertTrue(np.allclose(got.mean, ref.mean, atol=1e-9))
        self.assertTrue(np.allclose(got.std, ref.std, atol=1e-9))

    def test_jsonl_stream_trains(self):
        if not HAS_TORCH or not HAS_PANDAS:
            self.skipTest("torch/pandas required")
        from sgrn.ml.dataset import JsonlDatasetReader
        from sgrn.ml.pipeline import fitManifoldPipelineStream

        d = _tiny_frame(n=200)
        lines = [{"type": "dictionary",
                  "leaves": [{"id": i, "path": k} for i, k in
                             enumerate(["temp", "flow", "heater_on"])]}]
        for i in range(200):
            lines.append({"type": "anchor" if i == 0 else "delta",
                          "ts": d["ts"][i],
                          "data" if i == 0 else "changes": {
                              "0": float(d["temp"][i]), "1": float(d["flow"][i]),
                              "2": bool(d["heater_on"][i])}})
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.jsonl")
            with open(path, "w") as f:
                f.write("\n".join(json.dumps(line) for line in lines))
            out = fitManifoldPipelineStream(
                JsonlDatasetReader(path), features=["temp", "flow"],
                contextFeatures=["heater_on"], faultOnset=160, windowSize=8,
                modelConfig={"model": {"epochs": 1, "hidden_dim": 8,
                                       "latent_dim": 4, "alpha": 0.5}},
                modeHeadConfig={"epochs": 1, "hidden_dim": 8}, seed=11)
        self.assertGreater(out["nCalEffective"], 0)
        probe = np.zeros((8, 2), dtype=np.float32)
        ctx = np.empty((8, 1), dtype=object)
        ctx[:, 0] = True
        self.assertIn(out["detector"].detect(probe, ctx)["state"],
                      ("normal", "uncertain", "abnormal"))

    @unittest.skipUnless(HAS_ZSTD, "zstandard not installed")
    def test_binary_endianness_and_dtl(self):
        import struct
        import zlib

        import zstandard

        schema = {"dbs": [{"db_number": 1, "db_name": "M", "size_bytes": 20,
                            "endianness": "little", "fields": [
                                {"name": "v", "offset": 0, "type": "Real"},
                                {"name": "stamp", "offset": 4, "type": "DTL"}]}]}
        sj = json.dumps(schema).encode()
        img = bytearray(20)
        struct.pack_into("<f", img, 0, 7.5)
        img[4:6] = struct.pack("<H", 2024)
        img[6:8] = bytes([1, 2])
        pl = struct.pack("<HI", 1, zlib.crc32(bytes(img)) & 0xFFFFFFFF) + bytes(img)
        raw = b"SGRN" + struct.pack("<HI", 3, len(sj)) + sj
        raw += struct.pack("<QHI", 1700000000000, 0xFFFD, len(pl)) + pl
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.bin.zst")
            open(path, "wb").write(zstandard.ZstdCompressor().compress(raw))
            from sgrn.ml.dataset import BinaryDatasetReader

            rdr = BinaryDatasetReader(path)
            _, _, mat = rdr.readDataset()
            self.assertAlmostEqual(float(mat[0, 0]), 7.5, places=5)
            self.assertTrue(np.isnan(mat[0, 1]))
            df = rdr.readPandas()
            self.assertEqual(str(df["M.stamp"][0])[:10], "2024-01-02")


if __name__ == "__main__":
    unittest.main()
