#!/usr/bin/env python3
"""SGRN typed-twin workflow example: archive -> model -> time inference -> feature prediction.

End to end, no gateway required, no SCL file, no CSV intermediate:

  1. Synthesize a gateway-style JSONL archive (heater skid: temp/flow +
     heater_on/mode discretes, with a stuck-valve fault segment).
  2. Read it straight into a DataFrame (JsonlDatasetReader.readPandas).
  3. Train a manifold detector from the archive's own embedded schema
     (continuous targets + discrete conditioning + mode stratification).
  4. Infer plant state at two wall-clock times (nominal vs fault).
  5. Predict temp from the other signals in both regimes.
  6. Repeat training by streaming (O(window) memory) and compare.

Usage:
    python3 examples/twin_workflow.py [--epochs N] [--workdir DIR]
"""

import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "sgrn", "python"))

import numpy as np

from sgrn.ml.dataset import JsonlDatasetReader
from sgrn.ml.pipeline import (
    evaluateManifoldSequence,
    fitManifoldPipeline,
    fitManifoldPipelineStream,
    inferStateAt,
    predictFeature,
)

SCHEMA = {
    "dbs": [
        {
            "db_number": 1,
            "db_name": "Heater",
            "fields": [
                {"name": "temp", "type": "Real", "unit": "degC"},
                {"name": "flow", "type": "Real", "unit": "kg/h"},
                {"name": "heater_on", "type": "Bool"},
                {"name": "mode", "type": "USInt", "enum": {"0": "STOPPED", "1": "RUNNING"}},
            ],
        }
    ]
}

FEATURES = ["Heater.temp", "Heater.flow"]
CONTEXT = ["Heater.heater_on", "Heater.mode"]


def build_archive(path: str, n: int = 450, seed: int = 4) -> None:
    """Write a synthetic heater-skid JSONL archive with a stuck-valve fault."""
    rng = np.random.default_rng(seed)
    heat = np.array([True] * 200 + [False] * (n - 200))
    temp = np.where(heat, 280.0 + rng.normal(size=n) * 2.0, 20.0 + rng.normal(size=n))
    flow = 10.0 + 0.05 * np.where(heat, 280.0, 20.0) + rng.normal(size=n) * 0.2
    temp[350:370] = 280.0  # fault: hot metal while the heater reads OFF
    lines = [
        {"type": "schema", "schema": SCHEMA},
        {
            "type": "dictionary",
            "leaves": [
                {"id": 0, "path": "Heater.temp"},
                {"id": 1, "path": "Heater.flow"},
                {"id": 2, "path": "Heater.heater_on"},
                {"id": 3, "path": "Heater.mode"},
            ],
        },
    ]
    for i in range(n):
        lines.append(
            {
                "type": "anchor" if i == 0 else "delta",
                "ts": 1704067200000 + i * 1000,
                ("data" if i == 0 else "changes"): {
                    "0": float(temp[i]),
                    "1": float(flow[i]),
                    "2": bool(heat[i]),
                    "3": int(heat[i]),
                },
            }
        )
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(json.dumps(line) for line in lines))
    print(f"[1] archive written: {path} ({n} rows, fault at rows 350-370)")


def show_verdict(tag: str, verdict: dict) -> None:
    print(
        f"    {tag}: state={verdict['state']} "
        f"score={verdict['score']:.3f} "
        f"mismatch={verdict.get('mode_mismatch')}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--workdir", default="/tmp/opencode/sgrn_example")
    args = parser.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    archive = os.path.join(args.workdir, "heater.jsonl")
    model_cfg = {"model": {"epochs": args.epochs, "hidden_dim": 24,
                           "latent_dim": 6, "alpha": 0.10}}
    mode_cfg = {"epochs": max(25, args.epochs * 2), "hidden_dim": 16}

    build_archive(archive)

    # --- 2. archive -> DataFrame, schema from the archive itself ------------
    reader = JsonlDatasetReader(archive)
    df = reader.readPandas()
    print(f"[2] read {len(df)} rows straight from JSONL; "
          f"schema features: {reader.schema_json['dbs'][0]['db_name']}")

    # --- 3. train -----------------------------------------------------------
    out = fitManifoldPipeline(
        data=df, schemaSource=reader.schema_json, faultOnset=345,
        windowSize=8, modelConfig=dict(model_cfg), modeHeadConfig=dict(mode_cfg),
        seed=7,
    )
    det = out["detector"]
    print(f"[3] trained: features={det.features} context={det.context_features} "
          f"strata={out.get('strata')} q={det.q_alpha:.3f}")

    # --- 4. state at two wall-clock times ------------------------------------
    nominal_t = df["timestamp"][100]
    fault_t = df["timestamp"][360]
    r_nom = inferStateAt(det, df, nominal_t, contextData=df[CONTEXT])
    r_flt = inferStateAt(det, df, fault_t, contextData=df[CONTEXT])
    print("[4] infer state by time:")
    show_verdict(f"nominal t={nominal_t}", r_nom)
    show_verdict(f"fault   t={fault_t}", r_flt)
    assert r_nom["state"] == "normal", r_nom["state"]
    assert r_flt["state"] == "abnormal", r_flt["state"]

    # --- 5. predict temp from flow + mode ------------------------------------
    w_ok = df[FEATURES].iloc[100:108]
    p_ok = predictFeature(det, w_ok, "Heater.temp", context=df[CONTEXT].iloc[100:108])
    w_bad = df[FEATURES].iloc[353:361]
    p_bad = predictFeature(det, w_bad, "Heater.temp", context=df[CONTEXT].iloc[353:361])
    print("[5] predict Heater.temp from the other signals:")
    print(f"    nominal: actual={p_ok['actual']:.2f} predicted={p_ok['predicted']:.2f} "
          f"inside={p_ok['inside_interval']}")
    print(f"    fault:   actual={p_bad['actual']:.2f} predicted={p_bad['predicted']:.2f} "
          f"surprise={p_bad['surprise']:.1f}x inside={p_bad['inside_interval']}")
    assert p_ok["inside_interval"] and not p_bad["inside_interval"]

    # --- 6. same model, streamed (O(window) memory) --------------------------
    stream = fitManifoldPipelineStream(
        JsonlDatasetReader(archive), schemaSource=reader.schema_json,
        faultOnset=345, windowSize=8, modelConfig=dict(model_cfg),
        modeHeadConfig=dict(mode_cfg), shuffleBuffer=512, seed=7,
    )
    ds = stream["detector"]
    print(f"[6] streamed: q={ds.q_alpha:.3f} "
          f"(batch q={det.q_alpha:.3f}, nCal={stream['nCalEffective']})")
    rs = ds.detect(
        df[FEATURES].iloc[353:361].to_numpy(dtype=np.float32),
        {c: df[c].iloc[353:361].to_numpy() for c in CONTEXT},
    )
    print(f"    streamed verdict on fault window: {rs['state']} {rs['mode_mismatch']}")
    assert rs["state"] == "abnormal"

    # --- 7. range scoring ------------------------------------------------------
    res = evaluateManifoldSequence(det, df[FEATURES].iloc[350:371],
                                   contextData=df[CONTEXT].iloc[350:371])
    from collections import Counter
    print(f"[7] fault stretch states: {dict(Counter(res['states']))}")

    print("OK: archive -> model -> time inference -> feature prediction -> stream")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
