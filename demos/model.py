#!/usr/bin/env python3
"""
demos/model.py — Trustworthy-AI baseline for autonomous plant operations
                  on the Tennessee Eastman (TE) surrogate, wired to SGRN.

What `demos/model.py` launches (single entry point):
  1. TE surrogate          physics-informed synthetic plant (Downs & Vogel
                           nominal point + IDV fault subset, same tags as
                           sgrn/lib/gateway/simulations/tennessee/schema.scl)
  2. Simple ML models      PCA anomaly detector (monitor) +
                           RandomForest fault diagnoser (diagnose) +
                           Ridge/RF soft-sensor for reactor pressure (predict)
  3. Trustworthy layer     uncertainty quantification (ensemble std, entropy,
                           abstention) + doer-checker safety gate +
                           operator explanations + robustness stress tests
  4. SGRN export           dataset.csv + manifest.json (readable by
                           sgrn.ml.DatasetReader / AutoMLTrainer), model_meta.json,
                           report.md, optional live POST to the Gateway twin.

Usage:
  micromamba run -n SGRN python demos/model.py --mode full
  micromamba run -n SGRN python demos/model.py --mode train --n-per-fault 400
  micromamba run -n SGRN python demos/model.py --mode demo   # replay + doer-checker
  micromamba run -n SGRN python demos/model.py --mode full --push-gateway http://127.0.0.1:8000

No PyTorch needed for the baseline (sklearn only). The same feature pipeline
ports 1:1 to a PyTorch/UQ extension (see report.md "next steps").
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sgrn" / "python"))
from sgrn.ml import explainSample, fitMonitorAndDiagnoser

BASE_DIR = Path(__file__).resolve().parents[1]
OUT_DEFAULT = BASE_DIR / "scratch" / "te_demo"
SIM_DIR = BASE_DIR / "sgrn" / "lib" / "gateway" / "simulations" / "tennessee"

# --------------------------------------------------------------------------
# 1. TE surrogate definition (tag order = model feature order)
# --------------------------------------------------------------------------

# (twin path, unit, nominal, noise std)
FEATURES: list[tuple[str, str, float, float]] = [
    ("Reactor.a_feed_flow", "kscm", 0.25, 0.004),
    ("Reactor.d_feed_flow", "kg/h", 63.0, 0.4),
    ("Reactor.e_feed_flow", "kg/h", 54.0, 0.4),
    ("Reactor.ac_feed_flow", "kscm", 0.25, 0.004),
    ("Compressor.recycle_flow", "kscm", 32.0, 0.10),
    ("Reactor.reactor_feed_rate", "kscm", 42.0, 0.10),
    ("Reactor.pressure", "kPa", 2700.0, 4.0),
    ("Reactor.level", "%", 75.0, 0.15),
    ("Reactor.temp", "degC", 120.0, 0.10),
    ("Separator.purge_rate", "kscm", 0.34, 0.004),
    ("Separator.product_temp", "degC", 83.0, 0.10),
    ("Separator.sep_level", "%", 50.0, 0.15),
    ("Separator.sep_pressure", "kPa", 2600.0, 3.0),
    ("Separator.sep_underflow", "m3/h", 26.0, 0.10),
    ("Stripper.level", "%", 50.0, 0.15),
    ("Stripper.pressure", "kPa", 2600.0, 2.0),
    ("Stripper.underflow", "m3/h", 25.0, 0.10),
    ("Stripper.temp", "degC", 66.0, 0.10),
    ("Stripper.steam_flow", "kg/h", 230.0, 1.5),
    ("Compressor.work", "kW", 280.0, 1.0),
    ("Reactor.cooling_out_temp", "degC", 92.0, 0.15),
    ("Separator.cooling_out_temp", "degC", 88.0, 0.15),
    ("Reactor.feed_comp_a", "mol%", 32.0, 0.15),
    ("Reactor.feed_comp_b", "mol%", 14.0, 0.15),
    ("Reactor.feed_comp_c", "mol%", 22.0, 0.15),
    ("Reactor.feed_comp_d", "mol%", 10.0, 0.10),
    ("Reactor.feed_comp_e", "mol%", 18.0, 0.10),
    ("Reactor.feed_comp_f", "mol%", 2.0, 0.05),
    ("Separator.purge_comp_a", "mol%", 29.0, 0.15),
    ("Separator.purge_comp_b", "mol%", 13.0, 0.15),
    ("Separator.purge_comp_g", "mol%", 1.5, 0.05),
    ("Separator.purge_comp_h", "mol%", 0.5, 0.03),
    ("Stripper.product_comp_g", "mol%", 3.0, 0.05),
    ("Stripper.product_comp_h", "mol%", 1.0, 0.03),
]

FEATURE_NAMES = [f[0] for f in FEATURES]
NOMINAL = {f[0]: f[2] for f in FEATURES}
NOISE = {f[0]: f[3] for f in FEATURES}
TARGET = "Reactor.pressure"

# Fault subset: demo id -> (TE IDV, description, step offsets, extra noise mult)
FAULTS: dict[int, dict] = {
    0: {"idv": 0, "desc": "normal operation", "delta": {}, "nmult": 1.0},
    1: {"idv": 1, "desc": "A/C feed ratio step", "delta": {"Reactor.ac_feed_flow": 0.10, "Reactor.a_feed_flow": -0.05, "Reactor.pressure": 25.0}, "nmult": 1.0},
    2: {"idv": 2, "desc": "B composition step", "delta": {"Reactor.feed_comp_b": 3.5, "Reactor.feed_comp_a": -2.0}, "nmult": 1.0},
    3: {"idv": 3, "desc": "D feed temperature step", "delta": {"Reactor.d_feed_flow": 4.0, "Reactor.temp": 2.5}, "nmult": 1.0},
    4: {"idv": 4, "desc": "reactor cooling water step", "delta": {"Reactor.cooling_out_temp": 6.0, "Reactor.temp": 3.0, "Reactor.pressure": 35.0}, "nmult": 1.0},
    5: {"idv": 6, "desc": "A feed loss step", "delta": {"Reactor.a_feed_flow": -0.12, "Reactor.pressure": -30.0, "Reactor.level": -4.0}, "nmult": 1.2},
    6: {"idv": 7, "desc": "C header pressure loss", "delta": {"Reactor.ac_feed_flow": -0.08, "Compressor.recycle_flow": -3.0, "Reactor.pressure": -25.0}, "nmult": 1.2},
    7: {"idv": 8, "desc": "feed composition random variation", "delta": {}, "nmult": 3.0},
    8: {"idv": 11, "desc": "reactor cooling water random variation", "delta": {}, "nmult": 3.0},
}
TRAIN_FAULTS = [0, 1, 2, 3, 4, 5, 6]   # 7, 8 held out -> distribution-shift / unseen test
SAFETY = {"Reactor.pressure": 3000.0, "Reactor.temp": 150.0, "Reactor.level": 100.0}

ACTIONS = {
    0: "hold",
    1: "cut A feed",
    2: "open purge",
    3: "raise cooling",
    4: "cut steam",
    5: "safe-park (trip)",
}


# Latent process drivers (common-mode disturbances). Real plants move
# together: reaction intensity drives T/P/cooling/work, throughput drives
# flows/levels. This gives the normal manifold the PCA detector learns.
LATENT_LOAD = {
    "Reactor.temp": 1.5, "Reactor.pressure": 8.0, "Reactor.cooling_out_temp": 1.2,
    "Separator.sep_pressure": 6.0, "Separator.product_temp": 0.8,
    "Separator.cooling_out_temp": 1.0, "Compressor.work": 4.0,
    "Stripper.temp": 0.6, "Reactor.level": 0.4, "Separator.sep_level": 0.3,
    "Reactor.reactor_feed_rate": 0.3, "Compressor.recycle_flow": 0.5,
    "Separator.purge_rate": 0.004, "Stripper.steam_flow": 2.0,
}


def generateTe(n_per_fault: int, faults: list[int], seed: int = 7, dt_ms: int = 1000,
                warmup: int = 60):
    """Sequential AR(1)-regulated surrogate. Returns (df, info).

    Each scenario starts with a grade transition; the first `warmup` steps are
    simulated but NOT recorded (standard TE practice: exclude transitions).
    Without this, train/test transients dominate threshold calibration.
    """
    import numpy as np
    import pandas as pd

    rng = np.random.default_rng(seed)
    rows, labels, ts = [], [], []
    t = 0
    state = {k: v for k, v in NOMINAL.items()}
    react = 0.0  # latent reaction-intensity disturbance (slow AR(1))
    thru = 0.0   # latent throughput disturbance (slow AR(1))
    for fault in faults:
        spec = FAULTS[fault]
        # reset toward nominal at scenario start (grade transition)
        for k in state:
            state[k] += (NOMINAL[k] - state[k]) * 0.5
        react, thru = 0.0, 0.0
        for step in range(n_per_fault + warmup):
            t += dt_ms
            # Fast-mixing latents (tau ~3 steps): stationary within a run so
            # train/test regimes match; shift/surprise is carried by faults.
            react += (0.0 - react) * 0.30 + rng.normal(0, 0.60)
            thru += (0.0 - thru) * 0.30 + rng.normal(0, 0.40)
            for name in FEATURE_NAMES:
                pull = 0.15 if fault == 0 else 0.10  # faults persist: weaker pull
                target = (NOMINAL[name] + spec["delta"].get(name, 0.0)
                          + LATENT_LOAD.get(name, 0.0) * (react + thru))
                state[name] += (target - state[name]) * pull
                state[name] += rng.normal(0, NOISE[name] * spec["nmult"])
            # coupled physics: pressure follows temp/level/feed, work follows recycle
            state["Reactor.pressure"] += (state["Reactor.temp"] - NOMINAL["Reactor.temp"]) * 2.0 * 0.05
            state["Reactor.pressure"] += (state["Compressor.recycle_flow"] - NOMINAL["Compressor.recycle_flow"]) * 1.5 * 0.05
            state["Compressor.work"] += (state["Compressor.recycle_flow"] - NOMINAL["Compressor.recycle_flow"]) * 2.0 * 0.05
            state["Separator.sep_pressure"] += (state["Reactor.pressure"] - NOMINAL["Reactor.pressure"]) * 0.3 * 0.05
            if step < warmup:
                continue  # grade transition: simulate, don't record
            rows.append([state[n] for n in FEATURE_NAMES])
            labels.append(fault)
            ts.append(t)
    df = pd.DataFrame(rows, columns=FEATURE_NAMES)
    df["timestamp_ms"] = ts
    df["fault"] = labels
    return df


# --------------------------------------------------------------------------
# 2. Model bundle
# --------------------------------------------------------------------------

@dataclass
class Bundle:
    scaler: object
    pca: object
    det_threshold: float
    clf: object
    reg_ridge: object
    reg_rf: object
    reg_resid_std: float
    feature_names: list
    normal_mean: object


def trainBundle(df_train, df_cal):
    """Fit on df_train; calibrate the detection threshold on df_cal.

    df_cal MUST be normal-only data from an independent run (different seed).
    Calibrating on the training run itself underestimates cross-run variation
    (commissioning vs live data) and yields ~100% false alarms live — the
    classic mistake this baseline demonstrates and avoids.
    """
    import numpy as np
    from sklearn.ensemble import RandomForestRegressor
    from sklearn.linear_model import Ridge
    import warnings
    from sklearn.exceptions import ConvergenceWarning
    try:
        from sklearn.linear_model._ridge import LinAlgWarning
        warnings.simplefilter("ignore", LinAlgWarning)
    except ImportError:
        pass
    warnings.simplefilter("ignore", ConvergenceWarning)

    feat = FEATURE_NAMES
    scaler, pca, thr, clf, X_all, Xs, normal_mean = fitMonitorAndDiagnoser(
        df_train, df_cal, feat
    )

    # --- soft sensor: Reactor.pressure from all other tags ---
    ti = feat.index(TARGET)
    others = [i for i in range(len(feat)) if i != ti]
    Xo, yo = Xs[:, others], X_all[:, ti]
    ridge = Ridge().fit(Xo, yo)
    rf = RandomForestRegressor(n_estimators=200, random_state=7, n_jobs=-1).fit(Xo, yo)
    resid = yo - 0.5 * (ridge.predict(Xo) + rf.predict(Xo))
    rstd = float(resid.std() + 1e-9)

    return Bundle(scaler, pca, thr, clf, ridge, rf, rstd, feat, normal_mean)


def reconError(b: Bundle, X: object):
    import numpy as np
    Xs = b.scaler.transform(X)
    return ((Xs - b.pca.inverse_transform(b.pca.transform(Xs))) ** 2).mean(axis=1)


def predictWithUq(b: Bundle, X: object):
    """Returns dict with detection, diagnosis + confidences, pressure + std."""
    import numpy as np
    Xs = b.scaler.transform(X)
    re = ((Xs - b.pca.inverse_transform(b.pca.transform(Xs))) ** 2).mean(axis=1)
    proba = b.clf.predict_proba(Xs)
    pred = b.clf.classes_[proba.argmax(axis=1)]
    conf = proba.max(axis=1)
    ent = -(proba * np.log(proba + 1e-12)).sum(axis=1)
    ti = b.feature_names.index(TARGET)
    others = [i for i in range(len(b.feature_names)) if i != ti]
    pr_ridge = b.reg_ridge.predict(Xs[:, others])
    # RF tree-wise spread as epistemic uncertainty proxy
    rf_all = np.stack([t.predict(Xs[:, others]) for t in b.reg_rf.estimators_], axis=1)
    pr_mean = 0.5 * (pr_ridge + rf_all.mean(axis=1))
    pr_std = np.sqrt(rf_all.var(axis=1) + b.reg_resid_std ** 2)
    return {"recon": re, "fault": pred, "proba": proba, "conf": conf,
            "entropy": ent, "p_mean": pr_mean, "p_std": pr_std}


# --------------------------------------------------------------------------
# 3. Trustworthy layer: doer-checker + explanations
# --------------------------------------------------------------------------

def doerPolicy(fault: int, pressure: float, temp: float) -> int:
    """Doer: naive learned/LLM-style proposal (deliberately imperfect)."""
    if pressure > 2950 or temp > 145:
        return 5  # safe-park
    if fault == 4:
        return 3  # raise cooling — correct for cooling fault
    if fault in (1, 5):
        return 1  # cut A feed
    if fault == 6:
        return 2  # open purge
    if fault == 0:
        return 0
    return 0


def checker(action: int, p_mean: float, p_std: float, temp: float, level: float, conf: float) -> tuple[bool, str]:
    """Checker: independent physics + uncertainty gate. Returns (allow, reason)."""
    if conf < 0.60:
        return False, f"abstain: diagnosis confidence {conf:.2f} < 0.60, ask operator"
    hi = p_mean + 2 * p_std
    if hi > SAFETY["Reactor.pressure"]:
        if action not in (3, 5):
            return False, f"veto: predicted P95 {hi:.0f} kPa breaches {SAFETY['Reactor.pressure']:.0f}; only cooling/safe-park allowed"
    if temp > SAFETY["Reactor.temp"] or level > SAFETY["Reactor.level"]:
        if action not in (3, 5):
            return False, "veto: T/L envelope breached; only cooling/safe-park allowed"
    if action == 5 and p_mean < 2500 and temp < 130:
        return False, "veto: unnecessary trip would lose production; hold instead"
    return True, "pass: within envelope, uncertainty acceptable"


# --------------------------------------------------------------------------
# 4. Evaluation + SGRN export
# --------------------------------------------------------------------------

def evaluate(b: Bundle, df_test):
    import numpy as np
    from sklearn.metrics import accuracy_score, confusion_matrix, f1_score, mean_squared_error, r2_score

    X = df_test[FEATURE_NAMES].to_numpy(float)
    y = df_test["fault"].to_numpy(int)
    out = predictWithUq(b, X)
    det = (out["recon"] > b.det_threshold).astype(int)
    y_det = (y != 0).astype(int)
    det_rate = float((det[y_det == 1]).mean()) if (y_det == 1).any() else 1.0
    far = float((det[y_det == 0]).mean()) if (y_det == 0).any() else 0.0

    acc = float(accuracy_score(y, out["fault"]))
    f1 = float(f1_score(y, out["fault"], average="macro", zero_division=0))
    cm = confusion_matrix(y, out["fault"], labels=sorted(set(y) | set(b.clf.classes_)))
    known = np.isin(y, TRAIN_FAULTS)
    known_acc = float((out["fault"][known] == y[known]).mean()) if known.any() else 1.0

    ti = FEATURE_NAMES.index(TARGET)
    mse = float(mean_squared_error(X[:, ti], out["p_mean"]))
    r2 = float(r2_score(X[:, ti], out["p_mean"]))
    cal = float(np.mean(np.abs(X[:, ti] - out["p_mean"]) <= 2 * out["p_std"]))  # ~95% target

    # doer-checker audit on test set
    ti_t = FEATURE_NAMES.index("Reactor.temp")
    ti_l = FEATURE_NAMES.index("Reactor.level")
    blocked, unsafe_blocked, total_unsafe = 0, 0, 0
    for i in range(len(X)):
        a = doerPolicy(int(out["fault"][i]), float(out["p_mean"][i]), float(X[i, ti_t]))
        allow, _ = checker(a, float(out["p_mean"][i]), float(out["p_std"][i]),
                           float(X[i, ti_t]), float(X[i, ti_l]), float(out["conf"][i]))
        truly_unsafe = X[i, ti] > 2950 or X[i, ti_t] > 145
        if truly_unsafe:
            total_unsafe += 1
        if not allow:
            blocked += 1
            if truly_unsafe or a == 5:
                unsafe_blocked += 1
    return {"det_rate": det_rate, "far": far, "acc": acc, "f1": f1, "cm": cm,
            "known_acc": known_acc,
            "rmse": mse ** 0.5, "r2": r2, "coverage_p95": cal,
            "blocked_rate": blocked / max(1, len(X)), "unsafe_blocked": unsafe_blocked,
            "total_unsafe": total_unsafe, "out": out}


def robustnessTests(b: Bundle, df_test):
    """Sensor faults + shift not seen in training. Returns {name: (det, acc)}."""
    import numpy as np
    cases = {}
    X0 = df_test[FEATURE_NAMES].to_numpy(float)
    y0 = df_test["fault"].to_numpy(int)

    def score(Xm):
        o = predictWithUq(b, Xm)
        det = float((((o["recon"] > b.det_threshold).astype(int))[y0 != 0]).mean())
        acc = float((o["fault"] == y0).mean())
        return det, acc

    cases["clean"] = score(X0)
    Xb = X0.copy()
    Xb[:, FEATURE_NAMES.index("Reactor.pressure")] *= 1.05  # +5% bias
    cases["pressure_bias_+5pct"] = score(Xb)
    Xd = X0.copy()  # dropout -> mean imputation (what the twin would do)
    Xd[:, FEATURE_NAMES.index("Separator.sep_level")] = NOMINAL["Separator.sep_level"]
    cases["sep_level_dropout"] = score(Xd)
    Xn = X0.copy()
    rng = np.random.default_rng(0)
    Xn += rng.normal(0, 1, Xn.shape) * np.array([NOISE[n] for n in FEATURE_NAMES]) * 2.0
    cases["noise_x3"] = score(Xn)
    unseen = df_test[df_test["fault"].isin([7, 8])]
    if len(unseen):
        Xu = unseen[FEATURE_NAMES].to_numpy(float)
        o = predictWithUq(b, Xu)
        cases["unseen_faults_7_8_det"] = (float((o["recon"] > b.det_threshold).mean()), float("nan"))
    return cases


def exportSgrn(df_all, b: Bundle, metrics: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_p = out_dir / "dataset.csv"
    man_p = out_dir / "manifest.json"
    # NOTE: no string columns in the CSV — sgrn.ml.DatasetReader loads every
    # non-manifest column as float32. Split bookkeeping lives in the manifest.
    n_train = int((df_all["split"] == "train").sum())
    df_all.drop(columns=["split"]).to_csv(csv_p, index=False)
    manifest = {"features": [{"name": n, "unit": u, "is_categorical": False}
                             for n, u, _, _ in FEATURES] + [
                                 {"name": "timestamp_ms", "unit": "ms", "is_categorical": False},
                                 {"name": "fault", "unit": "id", "is_categorical": True}],
                "target": TARGET, "fault_map": {str(k): v["desc"] for k, v in FAULTS.items()},
                "n_train_rows": n_train,
                "det_threshold": b.det_threshold, "schema": "sgrn/lib/gateway/simulations/tennessee/schema.scl"}
    man_p.write_text(json.dumps(manifest, indent=2))
    (out_dir / "model_meta.json").write_text(json.dumps({
        "champion_detector": f"PCA(n={b.pca.n_components_}) thr={b.det_threshold:.4f}",
        "champion_diagnoser": "RandomForestClassifier(n=200)",
        "champion_soft_sensor": "Ridge+RandomForestRegressor ensemble",
        "metric_det_rate": metrics["det_rate"], "metric_far": metrics["far"],
        "metric_acc": metrics["acc"], "metric_f1": metrics["f1"],
        "metric_rmse_pressure": metrics["rmse"], "metric_r2_pressure": metrics["r2"],
        "metric_p95_coverage": metrics["coverage_p95"]}, indent=2))
    return csv_p, man_p


def writeReport(metrics: dict, robust: dict, out_dir: Path, n_train: int, n_test: int):
    lines = [
        "# TE trustworthy-AI baseline — run report",
        "",
        f"Train rows: {n_train} (faults {TRAIN_FAULTS}) | Test rows: {n_test} (incl. unseen 7, 8).",
        "",
        "## Monitor / diagnose / predict",
        f"- Detector (PCA on normal only): recall {metrics['det_rate']:.3f}, FAR {metrics['far']:.3f}, thr from 99th pct.",
        f"- Diagnoser (RF 200 trees): acc {metrics['acc']:.3f} overall, {metrics['known_acc']:.3f} on known faults, macro-F1 {metrics['f1']:.3f}.",
        f"- Soft-sensor Reactor.pressure (Ridge+RF): RMSE {metrics['rmse']:.2f} kPa, R2 {metrics['r2']:.3f}, P95 coverage {metrics['coverage_p95']:.3f}.",
        f"- Doer-checker: blocked {metrics['blocked_rate']:.2%} of proposals; unsafe situations seen: {metrics['total_unsafe']}.",
        "",
        "## Robustness (detection recall / accuracy)",
    ]
    for k, v in robust.items():
        lines.append(f"- {k}: det {v[0]:.3f}" + (f", acc {v[1]:.3f}" if v[1] == v[1] else ""))
    lines += [
        "",
        "## Confusion matrix (rows=true, cols=pred, label order sorted)",
        "```",
        str(metrics["cm"]),
        "```",
        "",
        "## Mapping to the EPFL IMOS proposal",
        "- Monitor/diagnose/act: detector -> diagnoser -> doer policy -> checker gate -> twin write.",
        "- Physics + twin: surrogate shares nominal point + tags with the SGRN SCL schema; swap in the",
        "  full TE Fortran/Simulink or P&ID-derived constraints without changing the pipeline.",
        "- Trustworthiness: ensemble-UQ + abstention, pre-execution checker vs 3000 kPa / 150 C / 100% envelope,",
        "  RF-deviation explanations, sensor-fault/shift/unseen-fault stress tests above.",
        "- Next (PhD): LLM agent with tool calls (twin read, historian, checker), conformal/UQ calibration,",
        "  formal checker envelope from P&IDs/procedures, closed-loop validation in the SGRN s7shell twin.",
        "",
        "## References",
        "- Downs & Vogel (1993), A plant-wide industrial process control problem, Comp. Chem. Eng.",
        "- Chiang, Russell & Braatz (2001), Fault Detection and Diagnosis in Industrial Systems.",
        "- The TEP IDV set + decentralized control structure; this surrogate keeps the nominal",
        "  point, tag semantics and fault IDs so results transfer to the full benchmark.",
    ]
    (out_dir / "report.md").write_text("\n".join(lines) + "\n")


def pushToGateway(b: Bundle, x_row, fault_pred: int, conf: float, url: str):
    payload = {"fault_code": int(fault_pred), "recommended_action":
               int(doerPolicy(fault_pred, 2700.0, 120.0)), "action_confidence": float(conf * 100)}
    req = urllib.request.Request(url.rstrip("/") + "/data/PlantWide",
                                 data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=3) as r:
        r.read()


# --------------------------------------------------------------------------
# 5. CLI modes
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="TE trustworthy-AI baseline wired to SGRN")
    ap.add_argument("--mode", choices=["full", "train", "demo"], default="full")
    ap.add_argument("--n-per-fault", type=int, default=400)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", type=str, default=str(OUT_DEFAULT))
    ap.add_argument("--push-gateway", type=str, default="")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    try:
        import numpy  # noqa
        import pandas  # noqa
        import sklearn  # noqa
    except ImportError as e:
        print(f"Missing dependency: {e}. Run inside the SGRN env:  micromamba run -n SGRN python demos/model.py")
        sys.exit(1)
    import warnings
    warnings.filterwarnings("ignore", message=".*ill-conditioned.*")

    out_dir = Path(args.out)
    print("=" * 60 + "\n SGRN x Tennessee Eastman — trustworthy-AI baseline\n" + "=" * 60)
    print(f"[1/5] Simulating TE surrogate (schema: {SIM_DIR.name}/schema.scl) ...")
    df_train = generateTe(args.n_per_fault, TRAIN_FAULTS, seed=args.seed)
    df_test = generateTe(max(150, args.n_per_fault // 2), [0, 1, 2, 3, 4, 5, 6, 7, 8], seed=args.seed + 1)
    print(f"      train {df_train.shape} faults={TRAIN_FAULTS} | test {df_test.shape} (7,8 unseen)")

    print("[2/5] Training detector + diagnoser + soft-sensor ...")
    df_cal = generateTe(args.n_per_fault, [0], seed=args.seed + 999)
    b = trainBundle(df_train, df_cal)

    print("[3/5] Evaluating (incl. doer-checker audit) ...")
    m = evaluate(b, df_test)
    print(f"      detector recall {m['det_rate']:.3f} FAR {m['far']:.3f} | "
          f"diagnoser acc {m['acc']:.3f} (known faults {m['known_acc']:.3f}) F1 {m['f1']:.3f}")
    print(f"      pressure soft-sensor RMSE {m['rmse']:.2f} kPa R2 {m['r2']:.3f} "
          f"P95-coverage {m['coverage_p95']:.3f}")
    print(f"      checker blocked {m['blocked_rate']:.2%} of actions")
    print("      confusion matrix (rows=true):\n", m["cm"])

    print("[4/5] Robustness stress (sensor faults / shift / unseen) ...")
    rob = robustnessTests(b, df_test)
    for k, v in rob.items():
        extra = f" acc {v[1]:.3f}" if v[1] == v[1] else ""
        print(f"      {k:24s} det {v[0]:.3f}{extra}")

    # operator-style explanation for one flagged sample
    import numpy as np
    X = df_test[FEATURE_NAMES].to_numpy(float)
    flagged = np.where(m["out"]["recon"] > b.det_threshold)[0]
    if len(flagged):
        i = int(flagged[0])
        print(f"[explain] first flagged test row {i} true fault {df_test['fault'].iloc[i]} "
              f"pred {m['out']['fault'][i]} conf {m['out']['conf'][i]:.2f}:")
        for name, s in explainSample(b, X[i]):
            print(f"      {name:32s} deviation-score {s:+.3f}")
        ti_t = FEATURE_NAMES.index("Reactor.temp")
        ti_l = FEATURE_NAMES.index("Reactor.level")
        a = doerPolicy(int(m["out"]["fault"][i]), float(m["out"]["p_mean"][i]), float(X[i, ti_t]))
        ok, why = checker(a, float(m["out"]["p_mean"][i]), float(m["out"]["p_std"][i]),
                          float(X[i, ti_t]), float(X[i, ti_l]), float(m["out"]["conf"][i]))
        print(f"      doer proposes '{ACTIONS[a]}' -> checker {'ALLOW' if ok else 'BLOCK'} ({why})")

    print("[5/5] Exporting SGRN artifacts ...")
    import pandas as pd
    df_all = pd.concat([df_train.assign(split="train"), df_test.assign(split="test")], ignore_index=True)
    csv_p, man_p = exportSgrn(df_all, b, m, out_dir)
    writeReport(m, rob, out_dir, len(df_train), len(df_test))
    print(f"      {csv_p}\n      {man_p}\n      {out_dir/'model_meta.json'}\n      {out_dir/'report.md'}")

    # sgrn.ml cross-check (same CSV through the real pipeline, if importable)
    sys.path.insert(0, str(BASE_DIR / "sgrn" / "python"))
    try:
        from sgrn.ml import AutoMLTrainer
        s = AutoMLTrainer(str(man_p), str(csv_p)).trainAndSelectBest(
            TARGET, task="regression", output_model_prefix=str(out_dir / "sgrn_model"))
        print(f"      [sgrn.ml] tournament champion: {s['champion_model']} R2 {s['metric_score']:.4f}")
    except Exception as e:
        print(f"      [sgrn.ml] skipped ({e})")

    if args.push_gateway:
        try:
            i = int(flagged[0]) if len(flagged) else 0
            pushToGateway(b, X[i], int(m["out"]["fault"][i]), float(m["out"]["conf"][i]), args.push_gateway)
            print(f"      pushed prediction to {args.push_gateway}/data/PlantWide")
        except Exception as e:
            print(f"      gateway push failed: {e}")

    if args.mode == "demo":
        print("\n[live replay] GroundTruth | Pred | P(kPa) | doer -> checker")
        for k in range(0, min(30, len(X)), 3):
            o = predictWithUq(b, X[k:k + 1])
            ti_t = FEATURE_NAMES.index("Reactor.temp")
            ti_l = FEATURE_NAMES.index("Reactor.level")
            a = doerPolicy(int(o["fault"][0]), float(o["p_mean"][0]), float(X[k, ti_t]))
            ok, _ = checker(a, float(o["p_mean"][0]), float(o["p_std"][0]),
                            float(X[k, ti_t]), float(X[k, ti_l]), float(o["conf"][0]))
            print(f"  t={int(df_test['timestamp_ms'].iloc[k]):7d} true={int(df_test['fault'].iloc[k])} "
                  f"pred={int(o['fault'][0])} P={float(o['p_mean'][0]):7.0f}±{float(o['p_std'][0]):4.0f} "
                  f"{ACTIONS[a]:10s} -> {'ALLOW' if ok else 'BLOCK'}")
            time.sleep(0.05)

    if not args.no_plot:
        try:
            import matplotlib.pyplot as plt
            fig, ax = plt.subplots(1, 2, figsize=(11, 4))
            ax[0].hist(m["out"]["recon"][df_test["fault"].to_numpy() == 0], bins=40, alpha=0.7, label="normal")
            ax[0].hist(m["out"]["recon"][df_test["fault"].to_numpy() != 0], bins=40, alpha=0.7, label="fault")
            ax[0].axvline(b.det_threshold, color="k", ls="--", label="thr")
            ax[0].set_title("PCA reconstruction error"); ax[0].legend()
            ti = FEATURE_NAMES.index(TARGET)
            ax[1].scatter(X[:, ti], m["out"]["p_mean"], s=4, alpha=0.4)
            ax[1].set_xlabel("true pressure kPa"); ax[1].set_ylabel("predicted kPa")
            ax[1].set_title("Soft-sensor parity")
            fig.tight_layout(); fig.savefig(out_dir / "parity.png", dpi=120)
            print(f"      {out_dir/'parity.png'}")
        except Exception as e:
            print(f"      plot skipped ({e})")

    print("=" * 60 + "\n done. Next: open scratch/te_demo/report.md for the 1-page EPFL summary.\n" + "=" * 60)


if __name__ == "__main__":
    main()
