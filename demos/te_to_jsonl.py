#!/usr/bin/env python3
"""Rieth et al. 2017 TE RData -> SGRN jsonl.zst (schema, dictionary, anchor+deltas).

SIMULATED DATA ONLY. Not a live plant.

Format follows documentation/gateway/jsonl-format.md:
  line1 schema {"type":"schema","schema":null} (null skips recovery validation)
  line2 dictionary {"type":"dictionary","leaves":[{"id","path"}]}
  line3 manifest {"type":"manifest",...}
  lines anchor/delta with numeric-ID keys, ts in simulated ms
  last  footer {"type":"footer",...}
Resolved by sgrn_replay JSONL branch via path_by_id (sgrn_replay.cpp).

Timestamps: simulated (sample-1)*180000 ms. Slow composition analysers
repeat values between updates; repeats emit no delta (exact compare).

Faulty files are large: loads one RData at a time, filters immediately.
Never holds all faulty files at once. Requires: pyreadr, zstandard, pandas.
"""

from __future__ import annotations

import argparse
import datetime
import io
import json
import sys
from pathlib import Path

FEATURES = [f"xmeas_{i}" for i in range(1, 42)] + [f"xmv_{i}" for i in range(1, 12)]
# Rieth column -> descriptive twin path (must match te_rieth/schema.scl order).
COLUMN_MAP = {
    "xmeas_1": "a_feed_flow", "xmeas_2": "d_feed_flow", "xmeas_3": "e_feed_flow",
    "xmeas_4": "ac_feed_flow", "xmeas_5": "recycle_flow", "xmeas_6": "reactor_feed_rate",
    "xmeas_7": "reactor_pressure", "xmeas_8": "reactor_level", "xmeas_9": "reactor_temp",
    "xmeas_10": "purge_rate", "xmeas_11": "sep_temp", "xmeas_12": "sep_level",
    "xmeas_13": "sep_pressure", "xmeas_14": "sep_underflow", "xmeas_15": "stripper_level",
    "xmeas_16": "stripper_pressure", "xmeas_17": "stripper_underflow", "xmeas_18": "stripper_temp",
    "xmeas_19": "stripper_steam_flow", "xmeas_20": "compressor_work",
    "xmeas_21": "reactor_cw_temp", "xmeas_22": "sep_cw_temp",
    "xmeas_23": "feed_comp_a", "xmeas_24": "feed_comp_b", "xmeas_25": "feed_comp_c",
    "xmeas_26": "feed_comp_d", "xmeas_27": "feed_comp_e", "xmeas_28": "feed_comp_f",
    "xmeas_29": "purge_comp_a", "xmeas_30": "purge_comp_b", "xmeas_31": "purge_comp_c",
    "xmeas_32": "purge_comp_d", "xmeas_33": "purge_comp_e", "xmeas_34": "purge_comp_f",
    "xmeas_35": "purge_comp_g", "xmeas_36": "purge_comp_h",
    "xmeas_37": "product_comp_d", "xmeas_38": "product_comp_e", "xmeas_39": "product_comp_f",
    "xmeas_40": "product_comp_g", "xmeas_41": "product_comp_h",
    "xmv_1": "d_feed_valve", "xmv_2": "e_feed_valve", "xmv_3": "a_feed_valve",
    "xmv_4": "ac_feed_valve", "xmv_5": "recycle_valve", "xmv_6": "purge_valve",
    "xmv_7": "sep_pot_valve", "xmv_8": "product_valve", "xmv_9": "steam_valve",
    "xmv_10": "reactor_cw_valve", "xmv_11": "condenser_valve",
}
# Twin feature order = schema.scl DB "TE" field order.
TWIN_FEATURES = [
    "a_feed_flow", "d_feed_flow", "e_feed_flow", "ac_feed_flow", "recycle_flow",
    "reactor_feed_rate", "purge_rate", "sep_underflow", "stripper_underflow",
    "stripper_steam_flow", "reactor_pressure", "sep_pressure", "stripper_pressure",
    "reactor_level", "sep_level", "stripper_level", "reactor_temp", "sep_temp",
    "stripper_temp", "reactor_cw_temp", "sep_cw_temp", "compressor_work",
    "feed_comp_a", "feed_comp_b", "feed_comp_c", "feed_comp_d", "feed_comp_e",
    "feed_comp_f", "purge_comp_a", "purge_comp_b", "purge_comp_c", "purge_comp_d",
    "purge_comp_e", "purge_comp_f", "purge_comp_g", "purge_comp_h",
    "product_comp_d", "product_comp_e", "product_comp_f", "product_comp_g",
    "product_comp_h", "d_feed_valve", "e_feed_valve", "a_feed_valve",
    "ac_feed_valve", "recycle_valve", "purge_valve", "sep_pot_valve",
    "product_valve", "steam_valve", "reactor_cw_valve", "condenser_valve",
]
LABEL_PATHS = ["TELabels.faultNumber", "TELabels.simulationRun"]
SAMPLE_PERIOD_MS = 180 * 1000


def load_frame(path: Path):
    import pyreadr

    res = pyreadr.read_r(str(path))
    if None in res:
        return res[None]
    return next(iter(res.values()))


def fail(msg: str) -> int:
    print(f"te_to_jsonl: error: {msg}", file=sys.stderr)
    return 1


def write_run(out: Path, run_df, leaf_ids: dict[str, int]) -> None:
    import zstandard as zstd

    buf = io.StringIO()
    buf.write(json.dumps({"type": "schema", "schema": None}) + "\n")
    leaves = [{"id": leaf_ids[f"TE.{c}"], "path": f"TE.{c}"} for c in TWIN_FEATURES]
    leaves += [{"id": leaf_ids[p], "path": p} for p in LABEL_PATHS]
    buf.write(json.dumps({"type": "dictionary", "leaves": leaves}) + "\n")
    buf.write(
        json.dumps(
            {
                "type": "manifest",
                "start_time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "mode": "changes_with_timestamp",
                "namespaces": ["TE"],
            }
        )
        + "\n"
    )
    prev: dict[str, float | int] = {}
    n = 0
    for _, row in run_df.iterrows():
        ts_ms = int((int(row["sample"]) - 1) * SAMPLE_PERIOD_MS)
        cur: dict[str, float | int] = {}
        for col, twin in COLUMN_MAP.items():
            cur[f"TE.{twin}"] = float(row[col])
        cur["TELabels.faultNumber"] = int(row["faultNumber"])
        cur["TELabels.simulationRun"] = int(row["simulationRun"])
        idmap = {str(leaf_ids[k]): v for k, v in cur.items()}
        if n == 0:
            buf.write(json.dumps({"type": "anchor", "ts": ts_ms, "data": idmap}) + "\n")
        else:
            changes = {k: v for k, v in idmap.items() if prev.get(k) != v}
            buf.write(json.dumps({"type": "delta", "ts": ts_ms, "changes": changes}) + "\n")
        prev = idmap
        n += 1
    buf.write(json.dumps({"type": "footer", "last_anchor_line": 4, "record_count": n + 4}) + "\n")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(zstd.ZstdCompressor(level=9).compress(buf.getvalue().encode()))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rdata", required=True, help="TEP_*.RData file")
    ap.add_argument("--split", choices=["train", "test"], required=True)
    ap.add_argument("--faults", type=int, nargs="*", default=None,
                    help="fault numbers to keep (default: all in file)")
    ap.add_argument("--runs", type=int, nargs="*", default=[1, 2, 3],
                    help="simulationRun values to keep (default: 1 2 3)")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    rdata = Path(args.rdata)
    if not rdata.is_file():
        return fail(f"RData not found: {rdata}")
    try:
        df = load_frame(rdata)
    except Exception as exc:  # keep CLI error style like demos/history_twin.py
        return fail(f"pyreadr failed on {rdata}: {exc}")

    for col in ("faultNumber", "simulationRun", "sample"):
        if col not in df.columns:
            return fail(f"column {col!r} missing; have {list(df.columns)[:8]}")
    for c in FEATURES:
        if c not in df.columns:
            return fail(f"feature column {c!r} missing")

    if args.faults is not None:
        df = df[df["faultNumber"].isin(args.faults)]
    if args.runs is not None:
        df = df[df["simulationRun"].isin(args.runs)]
    if len(df) == 0:
        return fail("selection is empty; check --faults/--runs")
    df = df.sort_values(["faultNumber", "simulationRun", "sample"]).reset_index(drop=True)

    paths = [f"TE.{c}" for c in TWIN_FEATURES] + LABEL_PATHS
    leaf_ids = {p: i for i, p in enumerate(paths)}
    out_dir = Path(args.out_dir)
    count = 0
    for (f, r), group in df.groupby(["faultNumber", "simulationRun"], sort=True):
        out = out_dir / f"te_{args.split}_f{int(f):02d}_run{int(r):03d}.jsonl.zst"
        write_run(out, group, leaf_ids)
        print(f"wrote f={int(f)} run={int(r)} rows={len(group)} -> {out}")
        count += 1
    print(f"done: {count} archives in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
