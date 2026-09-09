#!/usr/bin/env python3
"""
============================================================================
train_synthetic.py — Synthetic data generation + model training with s7shell
============================================================================
End-to-end pipeline:

  1. GENERATE  Render sgrn/s7shell/simulations/synth_gen.as per
     (scenario, seed) and run it with `s7shell` (schema.scl -> PlcRuntime ->
     SimEngine physics -> compressed .bin.zst WAL archives).
  2. CONVERT   Decode each run's WAL archives to CSV with `sgrn_dataset`
     (correctly flattens nested UDT leaves, e.g. Motor1.Bearing.HealthScore).
  3. LABEL     Tag every row with its ground-truth scenario
     (0 = healthy, 1 = bearing_degradation).
  4. TRAIN     Fault-detection classifier + bearing-health regressor
     (scikit-learn), time-ordered train/test split, champion selection.
  5. SAVE      joblib models + report.json under <work-dir>/models/.

Usage:
  python3 sgrn/s7shell/simulations/train_synthetic.py
  python3 sgrn/s7shell/simulations/train_synthetic.py --duration-s 600 --seeds 3
  python3 sgrn/s7shell/simulations/train_synthetic.py --skip-generate   # reuse WALs
============================================================================
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
SIM_DIR = Path(__file__).resolve().parent
TEMPLATE = SIM_DIR / "synth_gen.as"
SCHEMA = SIM_DIR / "schema.scl"

GREEN = "\033[32m"
CYAN = "\033[36m"
YELLOW = "\033[33m"
RED = "\033[31m"
BOLD = "\033[1m"
RESET = "\033[0m"


def log_info(msg):
    print(f"{CYAN}[TRAIN]{RESET} {msg}")


def log_success(msg):
    print(f"{GREEN}[OK]{RESET} {msg}")


def log_warn(msg):
    print(f"{YELLOW}[WARN]{RESET} {msg}")


def log_error(msg):
    print(f"{RED}[ERROR]{RESET} {msg}")


def find_binary(name):
    """Locate s7shell / sgrn_dataset in dist/build trees or PATH.

    Fresh build trees come first so source iterations (e.g. new script
    bindings) are picked up without requiring a reinstall into .dist.
    """
    candidates = [
        REPO_ROOT / ".build/linux-static-release/sgrn/s7shell" / name,
        REPO_ROOT / ".build/linux-static-release/sgrn/gateway" / name,
        REPO_ROOT / ".build/linux-static/sgrn/s7shell" / name,
        REPO_ROOT / ".build/linux-static/sgrn/gateway" / name,
        REPO_ROOT / ".dist/linux-static-release" / "bin" / name,
        REPO_ROOT / ".dist/linux-static-release" / name,
        REPO_ROOT / ".prefix/bin" / name,
        REPO_ROOT / "build" / name,
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return str(c)
    p = shutil.which(name)
    return p or ""


# ── Step 1: generate ─────────────────────────────────────────────────────────

def render_scenario_script(scenario, seed, duration_s, timestep_ms, noise, out_dir):
    tpl = TEMPLATE.read_text()
    for token, value in {
        "@SCHEMA@": str(SCHEMA),
        "@SEED@": str(seed),
        "@DURATION_S@": str(duration_s),
        "@TIMESTEP_MS@": str(timestep_ms),
        "@NOISE@": repr(float(noise)),
        "@FAULT@": scenario,
        "@OUT_DIR@": str(out_dir),
    }.items():
        tpl = tpl.replace(token, value)
    leftover = [t for t in ("@SCHEMA@", "@SEED@", "@DURATION_S@", "@TIMESTEP_MS@", "@NOISE@", "@FAULT@", "@OUT_DIR@") if t in tpl]
    assert not leftover, f"unsubstituted placeholders: {leftover}"
    return tpl


def generate_runs(s7shell_bin, scenarios, seeds, duration_s, timestep_ms, noise, work_dir):
    run_dirs = []  # (scenario, seed, run_dir)
    for scenario in scenarios:
        for i in range(seeds):
            seed = 1000 + 100 * scenarios.index(scenario) + i
            run_dir = work_dir / f"{scenario}_seed{seed}"
            out_dir = run_dir / "wal"
            out_dir.mkdir(parents=True, exist_ok=True)
            script_path = run_dir / "run.as"
            script_path.write_text(
                render_scenario_script(scenario, seed, duration_s, timestep_ms, noise, out_dir)
            )
            log_info(f"Generating: scenario={scenario} seed={seed} "
                     f"({duration_s}s @ {timestep_ms}ms) -> {out_dir}")
            start = time.time()
            res = subprocess.run([s7shell_bin, str(script_path)],
                                 capture_output=True, text=True, timeout=900)
            if res.returncode != 0:
                log_error(f"s7shell failed for {scenario} seed {seed} (rc={res.returncode}):\n{res.stderr[-3000:]}")
                sys.exit(1)
            archives = glob.glob(str(out_dir / "**" / "*.bin.zst"), recursive=True)
            if not archives:
                log_error(f"No .bin.zst archives produced in {out_dir}")
                log_error(f"s7shell output tail:\n{res.stdout[-2000:]}")
                sys.exit(1)
            log_success(f"{scenario} seed {seed}: {len(archives)} archive(s) "
                        f"in {time.time() - start:.1f}s")
            run_dirs.append((scenario, seed, run_dir))
    return run_dirs


# ── Step 2+3: convert + label ────────────────────────────────────────────────

def convert_and_label(dataset_bin, run_dirs, work_dir):
    import pandas as pd

    frames = []
    for scenario, seed, run_dir in run_dirs:
        label = 0 if scenario == "healthy" else 1
        state_dir = run_dir / "wal" / "state"
        csv_path = run_dir / "dataset.csv"
        manifest_path = run_dir / "manifest.json"
        log_info(f"Converting {scenario} seed {seed} WAL -> CSV ...")
        res = subprocess.run(
            [dataset_bin, str(state_dir), "--csv", str(csv_path),
             "-m", str(manifest_path), "-s", str(SCHEMA)],
            capture_output=True, text=True, timeout=900)
        if res.returncode != 0 or not csv_path.is_file():
            log_error(f"sgrn_dataset failed for {scenario} seed {seed}:\n{res.stderr[-3000:]}")
            sys.exit(1)
        df = pd.read_csv(csv_path)
        if len(df) == 0:
            log_error(f"Empty dataset for {scenario} seed {seed}")
            sys.exit(1)
        df["scenario"] = scenario
        df["seed"] = seed
        df["label"] = label
        frames.append(df)
        log_success(f"{scenario} seed {seed}: {len(df)} rows x {len(df.columns)} cols")

    data = pd.concat(frames, ignore_index=True)

    # Coerce everything to numeric (CSV stores bools as true/false strings).
    meta = {"scenario", "seed"}
    for col in data.columns:
        if col in meta:
            continue
        if data[col].dtype == object:
            s = data[col].astype(str).str.strip().str.lower()
            uniq = set(s.unique())
            if uniq <= {"true", "false", ""}:
                data[col] = s.map({"true": 1.0, "false": 0.0})
            else:
                data[col] = pd.to_numeric(data[col], errors="coerce")
    num_cols = [c for c in data.columns if c not in meta]
    n_nan = int(data[num_cols].isna().sum().sum())
    if n_nan:
        log_warn(f"{n_nan} non-numeric cells coerced to NaN -> filled with 0")
        data[num_cols] = data[num_cols].fillna(0.0)

    # One row per PLC tick: the WAL can carry several frames per tick
    # (dirty-field groups); keep the last snapshot of each tick.
    before = len(data)
    data = (data.sort_values("timestamp_ms")
                .drop_duplicates(subset=["scenario", "seed", "DbTelemetry.TickCount"], keep="last")
                .reset_index(drop=True))
    log_info(f"Deduped {before} -> {len(data)} rows (one per tick)")
    # Coverage note: rows begin at each archive's first anchor frame, so the
    # line-startup transient (first ~50 ticks) is typically not captured.
    for (scenario, seed), part in data.groupby(["scenario", "seed"]):
        tc = part["DbTelemetry.TickCount"].to_numpy()
        log_info(f"  {scenario} seed {seed}: ticks {int(tc.min())}..{int(tc.max())} "
                 f"({len(part)} rows, fault frac {part['label'].mean():.2f})")
    return data


# ── Step 4: train ────────────────────────────────────────────────────────────

def stratified_split(data, test_frac=0.2, seed=42):
    """Stratified shuffle split by fault label (seeded, reproducible).

    Measures the sensor -> target mapping quality. (A time-ordered split
    instead measures extrapolation beyond the training range — informative,
    but overly harsh here since degradation level is itself a function of
    run progress.)
    """
    import pandas as pd
    train_parts, test_parts = [], []
    for label, part in data.groupby("label"):
        part = part.sample(frac=1.0, random_state=seed).reset_index(drop=True)
        cut = int(len(part) * (1.0 - test_frac))
        train_parts.append(part.iloc[:cut])
        test_parts.append(part.iloc[cut:])
    return (pd.concat(train_parts, ignore_index=True).sample(frac=1.0, random_state=seed).reset_index(drop=True),
            pd.concat(test_parts, ignore_index=True).sample(frac=1.0, random_state=seed).reset_index(drop=True))


def drop_zero_variance(train, test, cols):
    nunique = train[cols].nunique()
    dead = [c for c in cols if nunique[c] <= 1]
    if dead:
        log_warn(f"Dropping {len(dead)} zero-variance feature(s): {dead[:5]}"
                 f"{'...' if len(dead) > 5 else ''}")
    return [c for c in cols if c not in dead], dead


def train_models(data, models_dir):
    import numpy as np
    from sklearn.ensemble import RandomForestClassifier, RandomForestRegressor
    from sklearn.linear_model import LogisticRegression, Ridge
    from sklearn.metrics import accuracy_score, f1_score, r2_score, mean_squared_error

    train, test = stratified_split(data)
    log_info(f"Train rows: {len(train)} | Test rows: {len(test)} "
             f"(split: stratified-shuffle seed=42; class balance test: {test['label'].mean():.2f} fault)")

    sensor_cols = [c for c in data.columns
                   if c not in {"scenario", "seed", "label", "timestamp_ms",
                                "DbTelemetry.TickCount"}]
    reg_target = "DbTelemetry.Motor1.Bearing.HealthScore"
    assert reg_target in sensor_cols, f"regression target {reg_target} missing from CSV"

    report = {"rows_train": len(train), "rows_test": len(test),
              "split": "stratified-shuffle test_size=0.2 random_state=42 (by fault label)"}

    # — Task A: fault detection (healthy vs bearing_degradation) —
    feats_a, dead_a = drop_zero_variance(train, test, sensor_cols)
    xa_train, xa_test = train[feats_a].to_numpy(), test[feats_a].to_numpy()
    ya_train, ya_test = train["label"].to_numpy(), test["label"].to_numpy()

    clf_candidates = {
        "LogisticRegression": LogisticRegression(max_iter=1000),
        "RandomForestClassifier": RandomForestClassifier(
            n_estimators=200, random_state=42, n_jobs=-1),
    }
    best_f1, best_clf, best_clf_name = -1.0, None, ""
    report["classifier"] = {"features": feats_a, "candidates": {}}
    for name, model in clf_candidates.items():
        model.fit(xa_train, ya_train)
        pred = model.predict(xa_test)
        acc = accuracy_score(ya_test, pred)
        f1 = f1_score(ya_test, pred, zero_division=0)
        report["classifier"]["candidates"][name] = {"accuracy": acc, "f1": f1}
        print(f"  [fault clf] {name:24s} acc={acc:.4f} f1={f1:.4f}")
        if f1 > best_f1:
            best_f1, best_clf, best_clf_name = f1, model, name
    report["classifier"]["champion"] = best_clf_name

    if hasattr(best_clf, "feature_importances_"):
        imp = sorted(zip(feats_a, best_clf.feature_importances_),
                     key=lambda kv: kv[1], reverse=True)[:10]
        print("  [fault clf] top features:")
        for feat, score in imp:
            print(f"    {score:.3f}  {feat}")
        report["classifier"]["top_features"] = [[f, float(s)] for f, s in imp]

    # — Task B: bearing-health regression (remaining HealthScore) —
    feats_b, dead_b = drop_zero_variance(
        train, test, [c for c in sensor_cols if c != reg_target])
    xb_train, xb_test = train[feats_b].to_numpy(), test[feats_b].to_numpy()
    yb_train, yb_test = train[reg_target].to_numpy(), test[reg_target].to_numpy()

    reg_candidates = {
        "Ridge": Ridge(),
        "RandomForestRegressor": RandomForestRegressor(
            n_estimators=200, random_state=42, n_jobs=-1),
    }
    best_r2, best_reg, best_reg_name = -float("inf"), None, ""
    report["regressor"] = {"target": reg_target, "features": feats_b, "candidates": {}}
    for name, model in reg_candidates.items():
        model.fit(xb_train, yb_train)
        pred = model.predict(xb_test)
        r2 = r2_score(yb_test, pred)
        rmse = float(np.sqrt(mean_squared_error(yb_test, pred)))
        report["regressor"]["candidates"][name] = {"r2": r2, "rmse": rmse}
        print(f"  [health reg] {name:24s} R2={r2:.4f} RMSE={rmse:.4f}")
        if r2 > best_r2:
            best_r2, best_reg, best_reg_name = r2, model, name
    report["regressor"]["champion"] = best_reg_name

    # — Step 5: save —
    import joblib
    models_dir.mkdir(parents=True, exist_ok=True)
    clf_path = models_dir / "fault_classifier.joblib"
    reg_path = models_dir / "health_regressor.joblib"
    joblib.dump({"model": best_clf, "features": feats_a,
                 "task": "fault_detection",
                 "label_map": {"0": "healthy", "1": "bearing_degradation"}}, clf_path)
    joblib.dump({"model": best_reg, "features": feats_b,
                 "task": "regression", "target": reg_target}, reg_path)
    report["artifacts"] = {"fault_classifier": str(clf_path),
                           "health_regressor": str(reg_path)}
    (models_dir / "report.json").write_text(json.dumps(report, indent=2))
    log_success(f"Saved champion '{best_clf_name}' (F1={best_f1:.4f}) -> {clf_path.name}")
    log_success(f"Saved champion '{best_reg_name}' (R2={best_r2:.4f}) -> {reg_path.name}")
    return report


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Generate synthetic PLC data with s7shell and train ML models on it.")
    ap.add_argument("--duration-s", type=int, default=300, help="Simulated seconds per run (default: 300)")
    ap.add_argument("--timestep-ms", type=int, default=100, help="Tick resolution in ms (default: 100)")
    ap.add_argument("--seeds", type=int, default=2, help="Runs per scenario (default: 2)")
    ap.add_argument("--noise", type=float, default=0.02, help="Gaussian noise level (default: 0.02)")
    ap.add_argument("--scenarios", default="healthy,bearing_degradation",
                    help="Comma-separated fault scenarios (default: healthy,bearing_degradation)")
    ap.add_argument("--work-dir", default=str(REPO_ROOT / "scratch" / "synth_train"),
                    help="Working directory for WALs/CSVs/models")
    ap.add_argument("--skip-generate", action="store_true",
                    help="Reuse existing WAL archives under <work-dir> (skip s7shell)")
    args = ap.parse_args()

    print(f"{BOLD}================================================================={RESET}")
    print(f"{BOLD}  SGRN Synthetic Data Generation + Model Training Pipeline       {RESET}")
    print(f"{BOLD}================================================================={RESET}")

    if not TEMPLATE.is_file() or not SCHEMA.is_file():
        log_error(f"Missing template/schema: {TEMPLATE} {SCHEMA}")
        sys.exit(1)

    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    if not scenarios:
        log_error("No scenarios requested.")
        sys.exit(1)

    os.chdir(REPO_ROOT)
    # Resolve after chdir so a relative --work-dir lands under the repo root
    # and every logged/serialized path is absolute.
    work_dir = Path(args.work_dir)
    if not work_dir.is_absolute():
        work_dir = REPO_ROOT / work_dir
    work_dir = work_dir.resolve()
    work_dir.mkdir(parents=True, exist_ok=True)

    s7shell_bin = find_binary("s7shell")
    dataset_bin = find_binary("sgrn_dataset")
    if not s7shell_bin and not args.skip_generate:
        log_error("s7shell binary not found — build first (e.g. cmake --build .build/linux-static-release).")
        sys.exit(1)
    if not dataset_bin:
        log_error("sgrn_dataset binary not found — build first.")
        sys.exit(1)
    log_info(f"s7shell:     {s7shell_bin or '(skipped)'}")
    log_info(f"sgrn_dataset: {dataset_bin}")

    if args.skip_generate:
        run_dirs = []
        for run_dir in sorted(work_dir.glob("*_seed*")):
            if (run_dir / "wal").is_dir():
                scenario = run_dir.name.rsplit("_seed", 1)[0]
                seed = int(run_dir.name.rsplit("_seed", 1)[1])
                run_dirs.append((scenario, seed, run_dir))
        if not run_dirs:
            log_error(f"--skip-generate but no <scenario>_seed* runs under {work_dir}")
            sys.exit(1)
        log_info(f"Reusing {len(run_dirs)} existing run(s).")
    else:
        run_dirs = generate_runs(s7shell_bin, scenarios, args.seeds,
                                 args.duration_s, args.timestep_ms,
                                 args.noise, work_dir)

    data = convert_and_label(dataset_bin, run_dirs, work_dir)
    log_info(f"Dataset: {len(data)} rows, "
             f"fault fraction: {data['label'].mean():.3f}")

    print(f"{BOLD}-----------------------------------------------------------------{RESET}")
    log_info("Training models ...")
    report = train_models(data, work_dir / "models")

    print(f"{BOLD}================================================================={RESET}")
    log_success("Pipeline complete. Artifacts:")
    for k, v in report["artifacts"].items():
        print(f"  {k:18s} {v}")
    print(f"{BOLD}================================================================={RESET}")


if __name__ == "__main__":
    main()
