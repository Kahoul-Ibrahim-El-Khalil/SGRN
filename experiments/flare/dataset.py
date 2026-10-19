#!/usr/bin/env python3
"""Dataset loader, chronological split, standardisation and sliding window generator.

Pipeline:
  1. Reconstruct signal state over time from archive / CSV.
  2. Select configured numeric process signals (excluding alarms, ESD, timestamps).
  3. Three-way chronological split of PRE-FAULT data:
     - train_ratio      of pre-fault -> training (scaler + autoencoder fit)
     - calibration_ratio of pre-fault -> conformal calibration (sets q_alpha ONLY)
     - heldout_ratio    of pre-fault -> held-out normal (FAR evaluation ONLY;
       never used for fitting, scaling or threshold choice)
     The fault day stays the test set in every mode.
  4. Standardisation fitted strictly on the train split.
  5. Sliding window generation.

Held-out provenance (``dataset.heldout_source``):
  - ``"split"``   — the chronological last ``heldout_ratio`` of the pre-fault
    data. These rows are cut from the SAME simulation as train/calibration, so
    the split blocks train/calibration contamination but the held-out rows are
    not a statistically independent process run (row-to-row autocorrelation
    spans the split boundary). The evaluation output records this explicitly.
  - ``"capture"`` — a second, INDEPENDENT pure-normal WAL run (produced with
    ``python3 experiments/flare capture --no-fault``) is used as the held-out
    set instead, and is preferred when available. NOTE: the bundled gas
    processing simulator seeds its noise LCG with a fixed start state
    (``rand_state = 12345`` in simulation.as) and exposes no seed override, so
    a re-capture replays the same trajectory; until the simulator is seeded,
    ``"split"`` is the only statistically meaningful option and this is stated
    in the evaluation output.
"""

from __future__ import annotations

import json
import subprocess
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Tuple

import numpy as np
import pandas as pd

from config_loader import loadConfig, getConfigPath, getWindowRows

ROOT = Path(__file__).resolve().parents[2]
SCL_SCHEMA = ROOT / "sgrn/lib/gateway/simulations/gas_processing/schema.scl"
CONFIG_PATH = getConfigPath(Path(__file__).parent)
META_PATH = Path(__file__).with_name("meta.json")


from sgrn.ml import Scaler, createSlidingWindows



def findSgrnDatasetBin() -> Path:
    paths = (
        ROOT / ".dist/linux-static-release/sgrn_dataset",
        ROOT / ".dist/linux-static/sgrn_dataset",
        ROOT / ".prefix/bin/sgrn_dataset",
    )
    for p in paths:
        if p.is_file():
            return p
    raise FileNotFoundError("sgrn_dataset binary not found")


def ensureWindowCache(archive_path: Path, cachePrefix: Path, exportCsv: Path | None = None) -> Path:
    """Stream an archive into the compact numeric cache (no CSV intermediate).

    Writes ``<cachePrefix>.f32`` (rows x features float32, feature order from
    ``<cachePrefix>.json``), ``<cachePrefix>.ts.i64`` (per-row WAL timestamps;
    INT64_MIN marks an unset timestamp) and ``<cachePrefix>.json`` metadata.
    When ``exportCsv`` is given, a human-readable CSV is produced alongside
    from the same pass (debug/inspection only).
    Skips the whole pass when the cache metadata already exists and is fresh.
    """
    meta_path = Path(str(cachePrefix) + ".json")
    if meta_path.is_file() and meta_path.stat().st_size > 0:
        return meta_path
    Path(cachePrefix).parent.mkdir(parents=True, exist_ok=True)

    bin_path = findSgrnDatasetBin()
    cmd = [
        str(bin_path),
        "-s", str(SCL_SCHEMA),
        "--cache", str(cachePrefix),
        "--manifest", str(cachePrefix.parent / "cache_manifest.json"),
        "-o", str(exportCsv) if exportCsv is not None else "",
        str(archive_path),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"sgrn_dataset cache stream failed: {res.stderr or res.stdout}")
    if not meta_path.is_file():
        raise RuntimeError(f"cache stream reported success but no metadata under {cachePrefix}")
    return meta_path


def loadCachedSeries(cachePrefix: Path) -> dict:
    """Read one cache: memmap values, timestamps, feature names/labels."""
    import numpy as np  # local: caller always has numpy
    meta = json.loads(Path(str(cachePrefix) + ".json").read_text(encoding="utf-8"))
    n_rows, n_feat = int(meta["rows"]), int(meta["features"])
    values = np.memmap(str(cachePrefix) + ".f32", dtype="<f4", mode="r", shape=(n_rows, n_feat))
    ts = np.fromfile(str(cachePrefix) + ".ts.i64", dtype="<i8", count=n_rows)
    return {
        "values": values,
        "ts": ts,
        "feature_names": list(meta["feature_names"]),
        "is_label": [bool(x) for x in meta.get("is_label", [False] * n_feat)],
        "rows": n_rows,
        "meta": meta,
    }


def forwardFillBounded(block: np.ndarray, limit: int) -> np.ndarray:
    """Per-column bounded forward fill (never looks forward — no leak)."""
    if limit <= 0:
        return block
    out = np.array(block, dtype=np.float32, copy=True)
    n_rows = out.shape[0]
    idx = np.arange(n_rows)
    for j in range(out.shape[1]):
        col = out[:, j]
        valid = ~np.isnan(col)
        if valid.all():
            continue
        last_valid = np.maximum.accumulate(np.where(valid, idx, -1))
        gap = np.where(last_valid >= 0, idx - last_valid, np.inf)
        fillable = (~valid) & (gap <= limit)
        if not fillable.any():
            continue
        out[:, j] = np.where(fillable, col[np.clip(last_valid, 0, n_rows - 1)], col)
    return out


def loadDatasetStream(archive_path: Path, cachePrefix: Path, features=None,
                      fillLimit: int = 5, exportCsv: Path | None = None,
                      naiveOffsetMs: int = 0) -> Tuple[np.ndarray, pd.Series, list[str]]:
    """Streaming replacement of loadDatasetDf: archive -> cache -> numeric matrix.

    Selects columns (auto = all non-label, non-control-blacklist names; or the
    configured list with leaf-name fallback), drops rows without an interior
    timestamp, applies bounded forward-fill, and renders simulated naive
    datetimes from the timestamps. ``naiveOffsetMs`` re-anchors ts from the
    run meta, but only applies to legacy caches whose rows carry WAL frame
    stamps; newer caches (``ts_domain == "dtl_wall_naive"``) already hold the
    datablocks' own DTL wall clock and are used as-is.
    """
    ensureWindowCache(archive_path, cachePrefix, exportCsv=exportCsv)
    cache = loadCachedSeries(cachePrefix)
    names = cache["feature_names"]
    offset_effective = 0 if cache["meta"].get("ts_domain") == "dtl_wall_naive" else int(naiveOffsetMs)

    # Rows without a resolvable timestamp (no datablock DTL, no frame stamp)
    # cannot enter chronological modeling.
    keep_row = cache["ts"] != np.iinfo(np.int64).min
    ts_ms = cache["ts"][keep_row]
    values = np.array(cache["values"][keep_row], dtype=np.float32, copy=True)
    n_dropped = int(len(cache["ts"]) - len(ts_ms))
    if n_dropped:
        print(f"  Dropping {n_dropped} rows with unset timestamps")

    control_blacklist = {
        "timestamp", "esd_signal", "model_trip_advice", "model_anomaly",
        "model_fault_code", "esd_manual", "esd_auto", "esd_reason_code",
        "esd_time_ms", "active_alarm_count", "any_critical", "dew_point_forecast",
        "dew_point_forecast_std", "dew_point_spec", "trip_high_press", "trip_low_press",
        "trip_high_temp", "trip_high_dp",
    }
    eligible = [
        i for i, n in enumerate(names)
        if not cache["is_label"][i]
        and n != "timestamp_ms"
        and n.split(".")[-1] not in control_blacklist
    ]

    if not features or features == "auto":
        col_idx = eligible
    else:
        # Match exact name first, then by unambiguous leaf suffix.
        col_idx = []
        for f in features:
            if f in names:
                col_idx.append(names.index(f))
                continue
            leaf = f.split(".")[-1]
            matches = [i for i in eligible if names[i].endswith("." + leaf) or names[i] == leaf]
            if len(matches) == 1:
                col_idx.append(matches[0])
            else:
                raise ValueError(f"Feature '{f}' not found in cache (available: {names[:10]}...)")

    block = np.ascontiguousarray(values[:, col_idx], dtype=np.float32)
    feature_names = [names[i] for i in col_idx]
    block = forwardFillBounded(block, fillLimit)

    sim_series = pd.Series(pd.to_datetime(ts_ms + offset_effective, unit="ms"))
    return block, sim_series, feature_names


def ensureDatasetCsv(archive_path: Path, csv_path: Path) -> Path:
    """Legacy CSV decode (debug/diff tooling only — pipelines use the stream API)."""
    cache_prefix = csv_path.parent / (csv_path.stem + "_cache")
    ensureWindowCache(archive_path, cache_prefix, exportCsv=csv_path)
    return csv_path


def resolveModelDir(model_dir: Path) -> Path:
    """Locate model.pt + model_meta.json under ``model_dir`` or its ``model/`` subfolder."""
    model_dir = Path(model_dir)
    if (model_dir / "model_meta.json").is_file():
        if model_dir.name != "model":
            print(f"[flare] NOTE: using flat artifact layout at {model_dir}")
        return model_dir
    sub = model_dir / "model"
    if (sub / "model_meta.json").is_file():
        print(f"[flare] NOTE: artifacts live in the 'model/' subfolder: {sub}")
        return sub
    raise FileNotFoundError(
        f"No model_meta.json under {model_dir} nor {sub}; "
        "train first (train_detector.py --save-dir <dir> writes <dir>/model/).")


def loadDatasetDf(csv_path: Path, features: Optional[list[str]] = None,
                    fillLimit: Optional[int] = 5) -> Tuple[pd.DataFrame, pd.Series]:
    df = pd.read_csv(csv_path)
    # Drop startup empty frame (row 0)
    df = df.iloc[1:].reset_index(drop=True)
    df = pd.read_csv(csv_path)
    # Drop startup empty frame (row 0)
    df = df.iloc[1:].reset_index(drop=True)

    # Simulated timestamps. Unset DTL fields render as year-0 strings
    # ("0000-00-00 ...") which no datetime parser accepts: coerce those to
    # NaT and drop the rows — a sample with no time has no temporal
    # position and cannot enter chronological modeling.
    if "InletSeparation.timestamp" in df.columns:
        sim_timestamps = pd.to_datetime(df["InletSeparation.timestamp"], errors="coerce")
    elif "timestamp_ms" in df.columns:
        sim_timestamps = pd.to_datetime(df["timestamp_ms"], unit="ms", errors="coerce")
    elif "_time" in df.columns:
        sim_timestamps = pd.to_datetime(df["_time"], errors="coerce")
    else:
        ts_col = next((c for c in df.columns if c.endswith(".timestamp")), df.columns[0])
        sim_timestamps = pd.to_datetime(df[ts_col], errors="coerce")
    n_bad_ts = int(sim_timestamps.isna().sum())
    if n_bad_ts:
        print(f"  Dropping {n_bad_ts} rows with unparseable timestamps (unset DTL renders as year 0)")
        keep = sim_timestamps.notna().to_numpy()
        df = df[keep].reset_index(drop=True)
        sim_timestamps = sim_timestamps[keep].reset_index(drop=True)
    if len(df) == 0:
        raise ValueError(f"No rows with usable timestamps in {csv_path}")

    control_blacklist = {
        "timestamp", "esd_signal", "model_trip_advice", "model_anomaly",
        "model_fault_code", "esd_manual", "esd_auto", "esd_reason_code",
        "esd_time_ms", "active_alarm_count", "any_critical", "dew_point_forecast",
        "dew_point_forecast_std", "dew_point_spec", "trip_high_press", "trip_low_press",
        "trip_high_temp", "trip_high_dp",
    }

    if not features or features == "auto":
        features = [
            c for c in df.columns
            if not c.startswith("_")
            and c.split(".")[-1] not in control_blacklist
            and pd.api.types.is_numeric_dtype(df[c])
        ]
    else:
        # Check for renames: match exact or match by leaf if unambiguous
        resolved_features = []
        for f in features:
            if f in df.columns:
                resolved_features.append(f)
            else:
                leaf = f.split(".")[-1]
                matches = [c for c in df.columns if c.endswith("." + leaf) or c == leaf]
                if len(matches) == 1:
                    resolved_features.append(matches[0])
                else:
                    raise ValueError(f"Feature '{f}' not found in CSV dataset (available: {list(df.columns[:10])}...)")
        features = resolved_features

    feature_df = df[features].copy()
    # Time-aware fill: bounded forward-fill only. Backward fill would copy
    # future observations into the past (temporal leak in replay scoring),
    # so leading gaps are left as NaN here and resolved split-aware below
    # with train-split medians (no test data involved).
    feature_df = feature_df.ffill(limit=fillLimit)
    return feature_df, sim_timestamps


def resolveTimestampOffset(meta: dict, cache_prefix: Path) -> int:
    """Effective ms offset re-anchoring cache row timestamps onto the meta clock.

    New caches declare ``ts_domain: "dtl_wall_naive"`` — their rows already
    hold the datablocks' DTL wall epoch, the same domain the meta onsets are
    expressed in, so the offset is 0 regardless of run meta. Legacy
    frame-stamped caches take the run meta's wall offset, with a
    fault_onset_iso-vs-fault_onset_ts fallback for metas that predate the key.
    """
    try:
        offset_ms = int(meta.get("sim_wall_naive_offset_ms", 0))
    except (TypeError, ValueError):
        offset_ms = 0
    if "sim_wall_naive_offset_ms" not in meta:
        try:
            onset_iso = pd.Timestamp(meta["fault_onset_iso"])
            onset_naive = onset_iso.tz_convert(None) if onset_iso.tzinfo is not None else onset_iso
            offset_ms = int((onset_naive.value - int(meta["fault_onset_ts"]) * 10**6) // 10**6)
        except (KeyError, TypeError, ValueError):
            offset_ms = 0
    try:
        cache_meta = json.loads(Path(str(cache_prefix) + ".json").read_text(encoding="utf-8"))
        if cache_meta.get("ts_domain") == "dtl_wall_naive":
            offset_ms = 0
    except (OSError, ValueError):
        pass
    return offset_ms


def loadLegacyCsvSplits(
    csv_path: Path, features_cfg, fill_limit: int, meta: dict
) -> Tuple[np.ndarray, list[str], pd.Series, np.ndarray, pd.Timestamp, np.ndarray]:
    """Legacy pandas-CSV route of getSplitsAndWindows (synthetic fixtures, debug).

    Returns ``(block, features, timestamps, ts_ms, fault_onset_dt,
    pre_fault_mask)``; the split is already expressed via fault_onset_dt,
    so callers report fault_onset_ts = 0.
    """
    feature_df, sim_timestamps = loadDatasetDf(csv_path, features_cfg, fillLimit=fill_limit)
    features = list(feature_df.columns)
    block = feature_df.to_numpy(dtype=np.float32)
    timestamps = sim_timestamps.dt.tz_localize(None)
    ts_ms = (timestamps.astype("datetime64[ns]").astype("int64").to_numpy() // 10**6)
    fault_onset_dt = pd.to_datetime(meta["fault_onset_iso"])
    fault_onset_dt = (fault_onset_dt.tz_convert(None) if getattr(fault_onset_dt, "tzinfo", None)
                      else fault_onset_dt.tz_localize(None))
    pre_fault_mask = np.asarray(timestamps < fault_onset_dt)
    return block, features, timestamps, ts_ms, fault_onset_dt, pre_fault_mask


def normalizeSplitRatios(dataset_cfg: dict) -> Tuple[float, float, float]:
    """Resolve (train, calibration, held-out) ratios of the PRE-FAULT split.

    Precedence:
      - ``heldout_ratio`` absent AND ``calibration_ratio`` absent → legacy
        two-way behaviour: remaining ``1 - train_ratio`` goes to calibration,
        held-out is empty (0.0).
      - one of the two present     → the other is the remainder of 1.
      - both present               → used verbatim; then sum must equal 1.

    Raises ``ValueError`` when ratios do not sum to 1 (within 1e-6) or leave
    [0, 1] — the split must be a complete, non-overlapping partition.
    """
    train_ratio = float(dataset_cfg.get("train_ratio", 0.70))
    cal_cfg = dataset_cfg.get("calibration_ratio")
    held_cfg = dataset_cfg.get("heldout_ratio")
    if cal_cfg is None and held_cfg is None:
        calibration_ratio = 1.0 - train_ratio
        heldout_ratio = 0.0
    else:
        calibration_ratio = 1.0 - train_ratio - (float(held_cfg) if held_cfg is not None else 0.0)
        if cal_cfg is not None:
            calibration_ratio = float(cal_cfg)
        heldout_ratio = 1.0 - calibration_ratio - train_ratio if held_cfg is None else float(held_cfg)

    total = train_ratio + calibration_ratio + heldout_ratio
    if abs(total - 1.0) > 1e-6:
        raise ValueError(
            f"Split ratios must sum to 1 (got train={train_ratio}, "
            f"calibration={calibration_ratio}, heldout={heldout_ratio}; sum={total:.6f})")
    for name, ratio in (("train", train_ratio), ("calibration", calibration_ratio),
                        ("heldout", heldout_ratio)):
        if not (0.0 <= ratio <= 1.0):
            raise ValueError(f"Split ratio '{name}' must lie in [0, 1]; got {ratio}")
    return train_ratio, calibration_ratio, heldout_ratio


def splitRowCounts(n_pre: int, ratios: Tuple[float, float, float],
                   window_size: int) -> Tuple[int, int, int]:
    """Integer row counts for the chronological pre-fault split.

    Train/calibration are floor-rounded; the held-out part absorbs the
    remainder so the parts sum to ``n_pre`` exactly. Every part whose ratio is
    nonzero must hold at least one full window (``window_size`` rows) — a
    split that cannot produce a single window anywhere is an invalid
    configuration, raised as ``ValueError`` instead of silently scored empty.
    """
    train_ratio, calibration_ratio, heldout_ratio = ratios
    n_train = int(n_pre * train_ratio)
    n_cal = int(n_pre * calibration_ratio)
    n_held = n_pre - n_train - n_cal
    for name, rows, ratio in (("train", n_train, train_ratio),
                              ("calibration", n_cal, calibration_ratio),
                              ("heldout", n_held, heldout_ratio)):
        if ratio > 0.0 and rows < window_size:
            raise ValueError(
                f"{name} split holds {rows} pre-fault rows but window_size={window_size}; "
                "it cannot yield even ONE full window. Adjust the split ratios, the "
                "window size, or capture more pre-fault data.")
    return n_train, n_cal, n_held


def resolveHeldoutArchive(config: dict, main_archive: Path, flare_dir: Path,
                          heldout_archive: Path | None = None) -> Path | None:
    """Locate the optional second, independent pure-normal WAL for the held-out set.

    Only consulted when ``dataset.heldout_source == "capture"`` (opt-in):
    candidates, first existing wins —
      1. the explicit ``heldout_archive`` argument,
      2. ``dataset.heldout_capture.archive`` resolved against the main
         archive's directory (run dirs), its ``heldout_capture/`` subfolder,
         and the experiment directory.
    Returns ``None`` when the mode is off or no file exists (the caller then
    falls back to the chronological pre-fault split and reports that fact).
    """
    ds_cfg = config.get("dataset", {})
    if str(ds_cfg.get("heldout_source", "split")) != "capture":
        return None
    name_cfg = ds_cfg.get("heldout_capture", {})
    name = (Path(str(name_cfg.get("archive", "flare_history_normal.jsonl.zst")))
            if isinstance(name_cfg, dict) else Path("flare_history_normal.jsonl.zst"))
    if heldout_archive is not None:
        candidates = [Path(heldout_archive)]
    else:
        target = name if name.is_absolute() else None
        if target is not None:
            candidates = [target]
        else:
            candidates = [
                Path(main_archive).parent / name,
                Path(main_archive).parent / "heldout_capture" / name,
                Path(flare_dir) / name,
            ]
    for cand in candidates:
        if cand.is_file():
            return cand
    print(f"  dataset.heldout_source='capture' but no held-out capture found "
          f"(looked in: {[str(c) for c in candidates]}); falling back to the "
          "chronological pre-fault split. Run: "
          "python3 experiments/flare capture --no-fault --out <dir>")
    return None


def loadHeldoutFromCapture(heldout_archive: Path, features: list[str], fill_limit: int,
                           train_median: np.ndarray) -> np.ndarray:
    """Raw held-out-normal block from a SECOND, independent pure-normal WAL run.

    Produced with ``python3 experiments/flare capture --no-fault``. The block
    is decoded with the SAME schema/features as the main run (identity of name
    AND order is asserted — a feature-list mismatch would silently misalign
    standardisation), forward-filled with the same bounded limit, and leading
    NaNs filled with the TRAIN split median so the held-out rows see no data
    from the calibration or test segments.

    Returns the raw engineering-unit block ``(n_rows, n_features)``.
    """
    cache_prefix = heldout_archive.parent / "window_cache_heldout" / "window_cache_heldout"
    block, _ts, feature_names = loadDatasetStream(
        heldout_archive, cache_prefix, features=features, fillLimit=fill_limit)
    if list(feature_names) != list(features):
        raise ValueError(
            "Held-out capture feature list does not match the main run "
            f"(heldout: {feature_names[:5]}..., expected {features[:5]}...)")
    block = np.array(block, dtype=np.float32, copy=True)
    for j in range(block.shape[1]):
        col = block[:, j]
        valid = np.nonzero(~np.isnan(col))[0]
        if len(valid) == 0:
            raise ValueError(f"Held-out capture column '{features[j]}' has no valid rows")
        n_lead = int(valid[0])
        if n_lead:
            col[:n_lead] = train_median[j]
    return block


def getSplitsAndWindows(
    archive_path: Path | None = None,
    cache_prefix: Path | None = None,
    export_csv: Path | None = None,
    config: dict | None = None,
    meta: dict | None = None,
    csv_path: Path | None = None,  # legacy arg: treated as export path
    heldout_archive: Path | None = None,  # explicit second normal WAL (overrides config lookup)
):
    if config is None:
        config = loadConfig(CONFIG_PATH)
    if meta is None:
        meta = json.loads(META_PATH.read_text(encoding="utf-8"))

    flare_dir = Path(__file__).parent
    if archive_path is None:
        archive_path = flare_dir / "flare_history.jsonl.zst"
    if cache_prefix is None:
        cache_prefix = archive_path.parent / "window_cache" / "window_cache"
    if csv_path is not None and export_csv is None:
        export_csv = csv_path

    features_cfg = config.get("features")
    fill_limit = config.get("dataset", {}).get("fill_limit", 5)
    dataset_cfg = config.get("dataset", {})

    # Legacy contract: an explicit csv_path that IS a CSV file routes through
    # the pandas path (synthetic test fixtures, debug tooling). Production
    # passes cache_prefix and goes through the streaming C++ cache below.
    legacy_csv = (csv_path is not None and Path(csv_path).suffix == ".csv"
                  and Path(csv_path).is_file())
    if legacy_csv:
        block, features, timestamps, ts_ms, fault_onset_dt, pre_fault_mask = loadLegacyCsvSplits(
            csv_path, features_cfg, fill_limit, meta)
        fault_onset_ts = 0  # split already expressed via fault_onset_dt above
    else:
        # Re-anchor legacy frame-stamped cache rows onto the meta's clock;
        # DTL-domain caches already resolve 0 (see resolveTimestampOffset).
        naive_offset_ms = resolveTimestampOffset(meta, cache_prefix)

        block, sim_dt, features = loadDatasetStream(
            archive_path, cache_prefix, features=features_cfg,
            fillLimit=fill_limit, exportCsv=export_csv, naiveOffsetMs=naive_offset_ms)
        timestamps = sim_dt

        # Split chronologically around fault onset (DTL wall-domain ms, no tz
        # games). Cast via ns keeps the ms→ns unit math dtype-independent
        # across pandas builds (datetime64[ms] Series astype("int64") already
        # yields ms).
        fault_onset_ts = int(meta["fault_onset_ts"])
        ts_ms = (timestamps.astype("datetime64[ns]").astype("int64").to_numpy() // 10**6) - naive_offset_ms
        pre_fault_mask = ts_ms < fault_onset_ts
    fault_mask = ~pre_fault_mask

    # ---- Three-way chronological split of PRE-FAULT data -------------------
    # train | calibration | held-out normal. The fault day remains the test
    # set. Ratios/rows are validated (sum to 1; every nonzero part yields at
    # least one full window).
    window_size = getWindowRows(config)
    ratios = normalizeSplitRatios(dataset_cfg)
    pre_fault_values = block[pre_fault_mask]
    fault_values = block[fault_mask]
    pre_fault_ts = timestamps[pre_fault_mask].reset_index(drop=True)
    fault_ts = timestamps[fault_mask].reset_index(drop=True)

    n_pre = len(pre_fault_values)
    if n_pre == 0:
        raise ValueError("No pre-fault rows before fault onset; check fault_onset_iso vs dataset range")

    n_train, n_cal, n_held = splitRowCounts(n_pre, ratios, window_size)
    train_ratio, calibration_ratio, heldout_ratio = ratios

    train_raw = np.array(pre_fault_values[:n_train], dtype=np.float32, copy=True)
    cal_raw = np.array(pre_fault_values[n_train:n_train + n_cal], dtype=np.float32, copy=True)
    heldout_raw = np.array(pre_fault_values[n_train + n_cal:], dtype=np.float32, copy=True)
    test_raw = np.array(fault_values, dtype=np.float32, copy=True)

    # Held-out provenance: "split" defaults to the chronological remainder
    # above; "capture" replaces it (below, after dead-feature filtering) with
    # rows from a second independent pure-normal WAL run when one is
    # available. The leftover pre-fault rows are then deliberately UNUSED
    # (counted, never scored) — they touch neither fitting nor threshold
    # choice.
    heldout_source = "chronological_split" if heldout_ratio > 0.0 else (
        None if len(heldout_raw) == 0 else "legacy_residual")
    unused_pre_fault_rows = 0
    heldout_archive_resolved = resolveHeldoutArchive(config, archive_path, flare_dir,
                                                     heldout_archive)

    # Leading gaps (unfilled by the bounded ffill) resolve against the
    # TRAIN split median only — test/calibration/held-out data never leak in.
    # Residual NaNs (outages longer than the fill limit) are left in place
    # and counted in the fill report instead of being paved over.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        train_median = np.nanmedian(train_raw, axis=0)
    dead = [features[j] for j in range(len(features)) if np.isnan(train_median[j])]

    def _drop_indices(block: np.ndarray, keep_idx: list[int]) -> np.ndarray:
        return np.ascontiguousarray(block[:, keep_idx], dtype=np.float32)

    if dead:
        if features_cfg:
            raise ValueError(f"Features with no valid training samples (dead sensors?): {dead}")
        # Auto-selected columns may legitimately include never-changing
        # inputs (alarms that never fired, enum placeholders). Drop them with a
        # report instead of aborting — the explicitly requested lists above
        # keep the strict behavior.
        print(f"  Dropping {len(dead)} dead features (all-NaN in training split), first: {dead[:5]}...")
        keep_idx = [j for j in range(len(features)) if not np.isnan(train_median[j])]
        features = [features[j] for j in keep_idx]
        train_raw = _drop_indices(train_raw, keep_idx)
        cal_raw = _drop_indices(cal_raw, keep_idx)
        heldout_raw = _drop_indices(heldout_raw, keep_idx)
        test_raw = _drop_indices(test_raw, keep_idx)

    def _fill_leading(block: np.ndarray) -> int:
        """Fill leading NaNs per column with the train median; return filled count."""
        filled = 0
        for j in range(block.shape[1]):
            col = block[:, j]
            valid = np.nonzero(~np.isnan(col))[0]
            if len(valid) == 0:
                continue  # all-NaN block column: left visible (train dead already rejected)
            n_lead = int(valid[0])
            if n_lead:
                col[:n_lead] = train_median[j]
                filled += n_lead
        return filled

    train_leading = _fill_leading(train_raw)
    _fill_leading(cal_raw)
    _fill_leading(test_raw)

    if heldout_archive_resolved is not None:
        # Independent capture held-out set: loaded AFTER dead-feature
        # filtering so its columns match the kept feature list exactly.
        heldout_source = "independent_capture"
        unused_pre_fault_rows = int(len(heldout_raw))
        heldout_raw = loadHeldoutFromCapture(
            heldout_archive_resolved, features, fill_limit, train_median)
        if len(heldout_raw) < window_size:
            raise ValueError(
                f"Held-out capture {heldout_archive_resolved} holds {len(heldout_raw)} rows "
                f"but window_size={window_size}; capture a longer --no-fault run.")
    heldout_leading = _fill_leading(heldout_raw)

    fill_report = {
        "fill_limit": config.get("dataset", {}).get("fill_limit", 5),
        "train_leading_filled": train_leading,
        "heldout_leading_filled": heldout_leading,
        "residual_nans": int(np.isnan(train_raw).sum() + np.isnan(cal_raw).sum()
                             + np.isnan(test_raw).sum() + np.isnan(heldout_raw).sum()),
    }

    # Fit scaler strictly on train_raw (NaN-aware: residual NaNs skipped)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(train_raw, axis=0)
        std = np.nanstd(train_raw, axis=0)
    std = np.where(std < 1e-4, 1.0, std)  # guard against constant features
    scaler = Scaler(mean=mean, std=std, feature_names=features)

    train_norm = scaler.transform(train_raw)
    cal_norm = scaler.transform(cal_raw)
    heldout_norm = scaler.transform(heldout_raw)
    test_norm = scaler.transform(test_raw)

    stride = config.get("dataset", {}).get("stride", 1)

    X_train = createSlidingWindows(train_norm, window_size, stride)
    X_cal = createSlidingWindows(cal_norm, window_size, stride)
    X_heldout = createSlidingWindows(heldout_norm, window_size, stride)

    # For test: include calibration tail so test windows start from the very first fault observation.
    # Note: cal_norm[-(window_size - 1):] with window_size=1 is the WHOLE array — slice explicitly.
    if window_size > 1 and len(cal_norm) > 0:
        test_context = np.vstack([cal_norm[max(0, len(cal_norm) - (window_size - 1)) :], test_norm])
    else:
        test_context = test_norm
    X_test = createSlidingWindows(test_context, window_size, stride)
    test_timestamps = fault_ts.tolist()

    return {
        "scaler": scaler,
        "features": features,
        "X_train": X_train,
        "X_cal": X_cal,
        "X_test": X_test,
        "X_heldout": X_heldout,
        "train_raw": train_raw,
        "cal_raw": cal_raw,
        "heldout_raw": heldout_raw,
        "test_raw": test_raw,
        "split_rows": {
            "train": int(len(train_raw)),
            "calibration": int(len(cal_raw)),
            "heldout": int(len(heldout_raw)),
            "unused_pre_fault_rows": unused_pre_fault_rows,
            "test": int(len(test_raw)),
        },
        "heldout_source": heldout_source,
        "heldout_archive": (
            str(heldout_archive_resolved) if heldout_archive_resolved is not None else None),
        "heldout_leading_filled": heldout_leading,
        "archive_path": archive_path,
        "timeline_values": block,
        "ts_ms": ts_ms,
        "timestamps": timestamps,
        "test_timestamps": test_timestamps,
        "fill_report": fill_report,
        "meta": meta,
        "config": config,
        "cache_prefix": cache_prefix,
    }


if __name__ == "__main__":
    splits = getSplitsAndWindows()
    print("Dataset processed successfully:")
    print(f"  Features: {len(splits['features'])}")
    print(f"  Fill report: {splits['fill_report']}")
    print(f"  Split rows: {splits['split_rows']} (heldout_source={splits['heldout_source']})")
    print(f"  X_train windows:   {splits['X_train'].shape}")
    print(f"  X_cal windows:     {splits['X_cal'].shape}")
    print(f"  X_test windows:    {splits['X_test'].shape}")
    print(f"  X_heldout windows: {splits['X_heldout'].shape}")
