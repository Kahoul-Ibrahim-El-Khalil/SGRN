#!/usr/bin/env python3
"""
demos/gas_processing.py — Full experiment on the gas-processing (molecular-sieve
dehydration) surrogate, wired to SGRN.

What `demos/gas_processing.py` launches (single entry point):
  1. Plant surrogate   3-tower adsorb/regen sequencer + heater/cooler PID,
                       dew-point physics and ESD logic mirroring
                       sgrn/lib/gateway/simulations/gas_processing/simulation.as
  2. Fault injection   7 faults (heater flame-out, stuck valve, pressure
                       surge, feed overload, blower failure, analyzer bias,
                       fast fouling) at tick T_INJ of every run — then the
                       script shows how the plant reacts (dew point, moisture,
                       heater, ESD trips).
  3. Predictor         H-step-ahead dew-point forecaster (Ridge vs RF ensemble
                       + std) + PCA anomaly detector (monitor) +
                       RF fault diagnoser (diagnose).
  4. Trustworthy layer uncertainty + abstention, doer-checker safety gate
                       (incl. false-trip veto on the analyzer-bias fault),
                       operator explanations, sensor-fault robustness tests.
  5. SGRN export       dataset.csv + manifest.json (readable by
                       sgrn.ml.DatasetReader / AutoMLTrainer), model_meta.json,
                       report.md, trajectory + parity plots, optional live POST
                       to a (mirror) Gateway twin.
  6. Twin mode         Gateway A (truth) + Gateway B (prediction mirror) driven
                       entirely through the repo bindings: sgrn.gateway for
                       writes (semantic REST incl. struct-array elements) and
                       sgrn.telemetry (WS subscribe -> dictionary decode ->
                       TelemetryEngine -> NumpyHistory) for the live push
                       channel. The predictor consumes subscribed twin tags.

Usage:
  micromamba run -n SGRN python demos/gas_processing.py --mode full
  micromamba run -n SGRN python demos/gas_processing.py --mode demo     # live replay of one run per fault
  micromamba run -n SGRN python demos/gas_processing.py --mode full --push-gateway http://127.0.0.1:18080
  micromamba run -n SGRN python demos/gas_processing.py --mode twin --fault 1
      # dual gateways: A=plant truth (:8080) + B=prediction mirror (:8082),
      # dashboards pop side by side, plant streams left, forecast right
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
OUT_DEFAULT = BASE_DIR / "scratch" / "gas_demo"

# Sequencer timings (ticks, 1 tick = 1 s, same as simulation.as)
T_DEPRESS, T_HEAT, T_COOL, T_REPRESS = 5, 15, 10, 5
DEW_SPEC = -100.0
H = 5               # forecast horizon (ticks ahead)
T_INJ = 40          # fault injection tick inside every run
RUN_STEPS = 140

# (twin path, unit, nominal, noise std, categorical?)
FEATURES: list[tuple[str, str, float, float, bool]] = [
    ("InletSeparation.feed_pressure", "bar", 68.0, 0.5, False),
    ("InletSeparation.feed_temp", "degC", 32.0, 0.2, False),
    ("InletSeparation.feed_flow", "dam3/h", 850.0, 15.0, False),
    ("InletSeparation.coalescer_dp", "bar", 0.15, 0.01, False),
    ("InletSeparation.scrubber_level", "%", 45.0, 0.5, False),
    ("AdsorberTowers.moisture_0", "%", 20.0, 0.3, False),
    ("AdsorberTowers.moisture_1", "%", 55.0, 0.3, False),
    ("AdsorberTowers.moisture_2", "%", 5.0, 0.3, False),
    ("AdsorberTowers.worst_moisture", "%", 55.0, 0.4, False),
    ("AdsorberTowers.regen_state", "state", 1.0, 0.0, True),
    ("AdsorberTowers.lead_lag_index", "index", 2.0, 0.0, True),
    ("AdsorberTowers.system_inlet_press", "bar", 68.0, 0.4, False),
    ("AdsorberTowers.system_outlet_press", "bar", 66.5, 0.4, False),
    ("RegenSystem.heater_outlet_temp", "degC", 35.0, 1.0, False),
    ("RegenSystem.heater_duty", "%", 0.0, 2.0, False),
    ("RegenSystem.heater_flame", "bool", 0.0, 0.0, True),
    ("RegenSystem.cooler_outlet_temp", "degC", 35.0, 0.5, False),
    ("RegenSystem.blower_flow", "dam3/h", 0.0, 5.0, False),
    ("RegenSystem.regen_water_removed", "l", 0.0, 0.05, False),
    ("OutletQuality.dew_point", "degC", -105.0, 0.3, False),  # measured (analyzer)
    ("OutletQuality.sales_flow", "dam3/h", 820.0, 10.0, False),
    ("SafetySystems.esd_signal", "bool", 0.0, 0.0, True),
    ("Utilities.fuel_gas_press", "bar", 3.2, 0.05, False),
]
FEATURE_NAMES = [f[0] for f in FEATURES]
NOMINAL = {f[0]: f[2] for f in FEATURES}
TARGET = "OutletQuality.dew_ahead"

FAULTS: dict[int, str] = {
    0: "normal operation",
    1: "heater flame-out during Heating",
    2: "regen-tower valve stuck open (stays online)",
    3: "feed pressure surge",
    4: "feed flow overload",
    5: "blower failure during regen",
    6: "dew-point analyzer bias +14C (sensor fault)",
    7: "coalescer fast fouling",
}
TRAIN_FAULTS = [0, 1, 2, 3, 4, 5]   # 6, 7 held out -> unseen / sensor-shift test
ENVELOPE = {"heater": 320.0, "feed_press": 90.0, "dew_margin": 5.0}
ACTIONS = {0: "hold", 1: "force regen switch", 2: "derate feed", 3: "trip ESD", 4: "request analyzer cal"}


# --------------------------------------------------------------------------
# 1. Plant surrogate (mirrors simulation.as, plus fault hooks)
# --------------------------------------------------------------------------

def runPlant(fault: int, steps: int, seed: int):
    """Simulate one run. Fault injected at T_INJ. Returns list of row dicts."""
    import numpy as np
    rng = np.random.default_rng(seed)
    n = lambda a: rng.normal(0, a)

    cycle = [0, 0, 1]
    t_state = [0.0, 0.0, 0.0]
    # near-cyclic steady state (a settled plant; no startup transient)
    moisture = [8.0, 12.0, 3.0]
    bed_dp = [0.20 + m / 100.0 * 0.6 for m in moisture]
    heat_ok = True  # latched per regen cycle: did Heating actually regenerate?
    lead = 2
    feed_press, feed_temp, feed_flow = 68.0, 32.0, 850.0
    coalescer_dp = 0.15
    heater_T, heater_duty, flame = 35.0, 0.0, False
    cooler_T, blower_flow, water_removed = 35.0, 0.0, 0.0
    esd = False
    rows = []

    for tick in range(steps):
        inj = tick >= T_INJ  # fault active window
        F = fault if inj else 0

        # --- sequencer ---
        for i in range(3):
            t_state[i] += 1.0
            if i == lead:
                if F == 2:
                    # stuck-open valve: the regen tower is never isolated, it
                    # keeps adsorbing (loads moisture) while counting as online
                    moisture[i] = min(95.0, moisture[i] + 0.05 + (0.02 if feed_flow > 850 else 0.0))
                    bed_dp[i] = 0.20 + moisture[i] / 100.0 * 0.6 + n(0.02)
                if cycle[i] == 1 and t_state[i] >= T_DEPRESS:
                    cycle[i] = 2; t_state[i] = 0.0
                elif cycle[i] == 2 and t_state[i] >= T_HEAT:
                    cycle[i] = 3; t_state[i] = 0.0
                elif cycle[i] == 3 and t_state[i] >= T_COOL:
                    cycle[i] = 4; t_state[i] = 0.0
                elif cycle[i] == 4 and t_state[i] >= T_REPRESS:
                    cycle[i] = 0; t_state[i] = 0.0
                    # Failed heating (flame-out) leaves the bed wet; a stuck
                    # valve means the bed was never isolated, so hot gas
                    # bypassed it -> also wet. Both ratchet into breakthrough.
                    if F == 2:
                        moisture[i] = 86.0
                    else:
                        moisture[i] = 3.0 if heat_ok else 88.0
                    bed_dp[i] = 0.20 + moisture[i] / 100.0 * 0.6
                    heat_ok = True
                    lead = (lead + 1) % 3
                    cycle[lead] = 1; t_state[lead] = 0.0
            else:
                load = 0.05 + (0.02 if feed_flow > 850 else 0.0)
                if F == 4:
                    load *= 3.5  # overload saturates beds fast
                moisture[i] = min(95.0, moisture[i] + load)
                bed_dp[i] = 0.20 + moisture[i] / 100.0 * 0.6 + n(0.02)

        # --- feeds ---
        base_flow = 1050.0 if F == 4 else 850.0
        base_press = 92.0 if F == 3 else 68.0
        feed_press = base_press + n(0.5)
        feed_flow = base_flow + n(15.0)
        foul_rate = 0.010 if F == 7 else 0.0001
        coalescer_dp = min(0.95, coalescer_dp + foul_rate)
        if F == 7:  # restriction sags the flow
            feed_flow -= 120.0

        # --- regen loop ---
        rs = cycle[lead]
        blower_ok = not (F == 5 and rs in (2, 3))
        if rs == 2:  # Heating
            flame = not (F == 1)  # flame-out fault
            if flame and blower_ok:
                err = 290.0 - heater_T
                heater_duty = min(100.0, max(0.0, 40.0 + err * 1.5 + n(2.0)))
                heater_T += err * 0.2 + n(1.0)
            elif not blower_ok:
                flame = True  # firing but no airflow -> runaway past 320 -> trip
                heater_duty = 80.0
                heat_ok = False  # no airflow: desorbed moisture not carried away
                heater_T += (380.0 - heater_T) * 0.15 + n(1.0)
            else:
                flame = False; heater_duty = 0.0
                heat_ok = False  # this regen cycle did not regenerate
                heater_T += (feed_temp - heater_T) * 0.2 + n(0.5)
            blower_flow = 220.0 + n(5.0) if blower_ok else 0.0
        elif rs == 3:  # Cooling
            flame = False; heater_duty = 0.0
            heater_T += (feed_temp - heater_T) * 0.2 + n(0.5)
            cooler_T += (35.0 - cooler_T) * 0.3 + n(0.5)
            blower_flow = (220.0 + n(5.0)) if blower_ok else 0.0
            if blower_ok:
                water_removed += 0.8
        else:
            flame = False; heater_duty = 0.0
            on = rs in (1, 4)
            blower_flow = (150.0 + n(2.0)) if (on and blower_ok) else 0.0
            cooler_T += (feed_temp - cooler_T) * 0.1

        # --- dew point: worst ONLINE tower (stuck valve keeps regen tower online) ---
        online = [i for i in range(3) if i != lead or F == 2]
        worst = max(moisture[i] for i in online)
        dew_true = -115.0 + worst / 95.0 * 25.0
        analyzer_bias = 14.0 if F == 6 else 0.0  # gross drift: forecaster sees it,
        dew_meas = dew_true + analyzer_bias + n(0.3)  # beds do not -> checker cross-check

        inlet_p = feed_press - 0.3
        outlet_p = inlet_p - 1.5 - sum(bed_dp) / 3.0

        # --- ESD (latching, same rule as simulation.as) ---
        if dew_meas > DEW_SPEC + 5.0 or feed_press > 90.0 or heater_T > 320.0:
            esd = True

        rows.append({
            "InletSeparation.feed_pressure": feed_press,
            "InletSeparation.feed_temp": feed_temp + n(0.2),
            "InletSeparation.feed_flow": feed_flow,
            "InletSeparation.coalescer_dp": coalescer_dp,
            "InletSeparation.scrubber_level": 45.0 + n(0.5),
            "AdsorberTowers.moisture_0": moisture[0],
            "AdsorberTowers.moisture_1": moisture[1],
            "AdsorberTowers.moisture_2": moisture[2],
            "AdsorberTowers.worst_moisture": worst,
            "AdsorberTowers.regen_state": float(rs),
            "AdsorberTowers.lead_lag_index": float(lead),
            "AdsorberTowers.system_inlet_press": inlet_p,
            "AdsorberTowers.system_outlet_press": outlet_p,
            "RegenSystem.heater_outlet_temp": heater_T,
            "RegenSystem.heater_duty": heater_duty,
            "RegenSystem.heater_flame": float(flame),
            "RegenSystem.cooler_outlet_temp": cooler_T,
            "RegenSystem.blower_flow": blower_flow,
            "RegenSystem.regen_water_removed": water_removed,
            "OutletQuality.dew_point": dew_meas,
            "OutletQuality.sales_flow": 820.0 + n(10.0),
            "SafetySystems.esd_signal": float(esd),
            "Utilities.fuel_gas_press": 3.2 + n(0.05),
            "dew_true": dew_true,
            "cycle_0": float(cycle[0]), "cycle_1": float(cycle[1]), "cycle_2": float(cycle[2]),
            "bed_dp_0": bed_dp[0], "bed_dp_1": bed_dp[1], "bed_dp_2": bed_dp[2],
            "fault": fault, "tick": tick,
            "post": int(tick >= T_INJ),
        })
    return rows


def generateGas(runs_per_fault: int, faults: list[int], seed: int):
    import pandas as pd
    rows = []
    for fi, fault in enumerate(faults):
        for r in range(runs_per_fault):
            for row in runPlant(fault, RUN_STEPS, seed + 100 * fi + r):
                row["run"] = f"f{fault}r{r}"
                rows.append(row)
    df = pd.DataFrame(rows)
    # H-step-ahead measured dew target (within-run shift; tail NaN -> drop)
    df[TARGET] = df.groupby("run")["OutletQuality.dew_point"].shift(-H)
    df = df.dropna(subset=[TARGET]).reset_index(drop=True)
    df["timestamp_ms"] = (df["tick"] + 1) * 1000
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
    import warnings
    import numpy as np
    from sklearn.ensemble import RandomForestRegressor
    from sklearn.linear_model import Ridge
    warnings.filterwarnings("ignore", message=".*ill-conditioned.*")

    feat = FEATURE_NAMES
    scaler, pca, thr, clf, X_all, Xs, normal_mean = fitMonitorAndDiagnoser(
        df_train, df_cal, feat
    )

    yo = df_train[TARGET].to_numpy(float)   # dew H ticks ahead
    # Tournament: linear Ridge vs nonlinear RF. The twin tags carry a discrete
    # sequencer state machine, so the linear model cannot compete — it is kept
    # for the tournament record, but the deployed champion is the RF (whose
    # tree spread also gives the uncertainty band).
    ridge = Ridge(alpha=25.0).fit(Xs, yo)
    rf = RandomForestRegressor(n_estimators=200, random_state=7, n_jobs=-1).fit(Xs, yo)
    rstd = float((yo - rf.predict(Xs)).std() + 1e-9)
    return Bundle(scaler, pca, thr, clf, ridge, rf, rstd, feat, normal_mean)


def predictWithUq(b: Bundle, X: object):
    import numpy as np
    Xs = b.scaler.transform(X)
    re = ((Xs - b.pca.inverse_transform(b.pca.transform(Xs))) ** 2).mean(axis=1)
    proba = b.clf.predict_proba(Xs)
    pred = b.clf.classes_[proba.argmax(axis=1)]
    conf = proba.max(axis=1)
    rf_all = np.stack([t.predict(Xs) for t in b.reg_rf.estimators_], axis=1)
    return {"recon": re, "fault": pred, "conf": conf,
            "dew_mean": rf_all.mean(axis=1),   # champion = RF (see trainBundle)
            "dew_std": np.sqrt(rf_all.var(axis=1) + b.reg_resid_std ** 2)}


# --------------------------------------------------------------------------
# 3. Trustworthy layer
# --------------------------------------------------------------------------

def doerPolicy(fault: int, dew_pred: float, heater: float, press: float) -> int:
    if dew_pred > DEW_SPEC + 5.0 or heater > 315.0 or press > 89.0:
        return 3  # trip ESD
    if fault == 2:
        return 1  # force regen switch (stuck valve)
    if fault == 4:
        return 2  # derate feed (overload)
    if fault == 6:
        return 4  # request analyzer calibration, keep running
    if fault == 1:
        return 1  # re-try regen on next tower
    return 0


def checker(action: int, dew_mean: float, dew_std: float, heater: float,
            press: float, conf: float, moisture_xcheck: float) -> tuple[bool, str]:
    if conf < 0.60:
        return False, f"abstain: confidence {conf:.2f} < 0.60, ask operator"
    hi = dew_mean + 2 * dew_std
    # independent physics cross-check: moisture-implied dew (immune to analyzer bias)
    dew_implied = -115.0 + moisture_xcheck / 95.0 * 25.0
    if action == 3 and dew_implied < DEW_SPEC + 2.0 and heater < 300.0 and press < 88.0 \
            and (dew_mean - dew_implied) > 4.0:
        return False, (f"veto false trip: analyzer says {dew_mean:.1f}C but beds imply "
                       f"{dew_implied:.1f}C — likely sensor fault, calibrate instead")
    if hi > DEW_SPEC + 5.0 or heater > ENVELOPE["heater"] or press > ENVELOPE["feed_press"]:
        if action not in (3,):
            return False, f"veto: envelope breach predicted (dew95 {hi:.1f}C) — only ESD allowed"
    return True, "pass: within envelope, uncertainty acceptable"


# --------------------------------------------------------------------------
# 4. Evaluation + SGRN export
# --------------------------------------------------------------------------

def evaluate(b: Bundle, df_test):
    import numpy as np
    from sklearn.metrics import accuracy_score, confusion_matrix, f1_score, mean_squared_error, r2_score

    X = df_test[FEATURE_NAMES].to_numpy(float)
    y = df_test["fault"].to_numpy(int)
    post = df_test["post"].to_numpy(int)
    out = predictWithUq(b, X)
    det = (out["recon"] > b.det_threshold).astype(int)

    post_fault = (post == 1) & (y != 0)
    pre_or_normal = (post == 0) | (y == 0)
    det_rate = float(det[post_fault].mean()) if post_fault.any() else 1.0
    far = float(det[pre_or_normal].mean()) if pre_or_normal.any() else 0.0

    acc = float(accuracy_score(y, out["fault"]))
    f1 = float(f1_score(y, out["fault"], average="macro", zero_division=0))
    cm = confusion_matrix(y, out["fault"], labels=sorted(set(y) | set(b.clf.classes_)))
    known = np.isin(y, TRAIN_FAULTS)
    known_acc = float((out["fault"][known] == y[known]).mean())

    yt = df_test[TARGET].to_numpy(float)
    rmse = float(mean_squared_error(yt, out["dew_mean"]) ** 0.5)
    r2 = float(r2_score(yt, out["dew_mean"]))
    cal = float(np.mean(np.abs(yt - out["dew_mean"]) <= 2 * out["dew_std"]))

    # plant reaction per fault: ESD rate, breakthrough rate, peak dew, time-to-detect
    reaction = {}
    for f in sorted(df_test["fault"].unique()):
        d = df_test[df_test["fault"] == f]
        det_runs = det[d.index.to_numpy()]
        ttd = []
        for run, g in d.groupby("run"):
            idx = np.where(det[g.index.to_numpy()] & (g["post"].to_numpy() == 1))[0]
            ttd.append(int(g["tick"].iloc[idx[0]] - T_INJ) if len(idx) else None)
        post_mask = (d["post"].to_numpy() == 1)
        rec_f = float(det_runs[post_mask].mean()) if post_mask.any() else 1.0
        valid_ttd = [t for t in ttd if t is not None]
        ef = yt[d.index.to_numpy()] - out["dew_mean"][d.index.to_numpy()]
        reaction[f] = {
            "recall": rec_f,
            "rmse_f": float((ef ** 2).mean() ** 0.5),
            "esd_rate": float(d.groupby("run")["SafetySystems.esd_signal"].max().mean()),
            "breakthrough_rate": float((d.groupby("run")["AdsorberTowers.worst_moisture"]
                                        .max() > 90.0).mean()),
            "peak_dew": float(d["OutletQuality.dew_point"].max()),
            "ttd_med": float(np.median(valid_ttd)) if valid_ttd and f != 0 else float("nan"),
            "ttd_miss": int(sum(t is None for t in ttd)),
        }

    # doer-checker audit
    blocked = unsafe_blocked = total_unsafe = vetos_false_trip = 0
    for i in range(len(X)):
        r = df_test.iloc[i]
        a = doerPolicy(int(out["fault"][i]), float(out["dew_mean"][i]),
                        float(r["RegenSystem.heater_outlet_temp"]),
                        float(r["InletSeparation.feed_pressure"]))
        ok, why = checker(a, float(out["dew_mean"][i]), float(out["dew_std"][i]),
                          float(r["RegenSystem.heater_outlet_temp"]),
                          float(r["InletSeparation.feed_pressure"]),
                          float(out["conf"][i]), float(r["AdsorberTowers.worst_moisture"]))
        truly_bad = (r["OutletQuality.dew_point"] > DEW_SPEC + 5.0
                     or r["RegenSystem.heater_outlet_temp"] > 320.0
                     or r["InletSeparation.feed_pressure"] > 90.0)
        total_unsafe += int(truly_bad)
        if not ok:
            blocked += 1
            unsafe_blocked += int(truly_bad or a == 3)
            vetos_false_trip += int("false trip" in why)
    return {"det_rate": det_rate, "far": far, "acc": acc, "f1": f1, "cm": cm,
            "known_acc": known_acc, "rmse": rmse, "r2": r2, "coverage": cal,
            "reaction": reaction, "blocked_rate": blocked / max(1, len(X)),
            "unsafe_blocked": unsafe_blocked, "total_unsafe": total_unsafe,
            "false_trip_vetos": vetos_false_trip, "out": out}


def robustnessTests(b: Bundle, df_test):
    X0 = df_test[FEATURE_NAMES].to_numpy(float)
    yt = df_test[TARGET].to_numpy(float)

    def score(Xm):
        import numpy as np
        o = predictWithUq(b, Xm)
        post = (df_test["post"].to_numpy() == 1) & (df_test["fault"].to_numpy() != 0)
        return (float(((o["recon"] > b.det_threshold).astype(int))[post].mean()),
                float((np.abs(yt - o["dew_mean"]) ** 2).mean() ** 0.5))

    cases = {"clean": score(X0)}
    Xb = X0.copy()  # analyzer drift +3C on top of everything
    Xb[:, FEATURE_NAMES.index("OutletQuality.dew_point")] += 3.0
    cases["analyzer_drift_+3C"] = score(Xb)
    Xd = X0.copy()  # pressure tap frozen -> twin holds last good (mean here)
    Xd[:, FEATURE_NAMES.index("InletSeparation.feed_pressure")] = NOMINAL["InletSeparation.feed_pressure"]
    cases["press_tap_frozen"] = score(Xd)
    import numpy as np
    Xn = X0.copy() + np.random.default_rng(0).normal(
        0, 1, X0.shape) * np.array([dict((n, s) for n, _, _, s, _ in FEATURES)[n]
                                    for n in FEATURE_NAMES]) * 2.0
    cases["noise_x3"] = score(Xn)
    return cases


def exportSgrn(df_all, b: Bundle, metrics: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_p, man_p = out_dir / "dataset.csv", out_dir / "manifest.json"
    keep = FEATURE_NAMES + [TARGET, "timestamp_ms", "fault",
                            "SafetySystems.esd_signal", "tick", "post", "dew_true"]
    df_all[keep].to_csv(csv_p, index=False)
    manifest = {"features": [{"name": n, "unit": u, "is_categorical": bool(c)}
                             for n, u, _, _, c in FEATURES]
                + [{"name": TARGET, "unit": "degC", "is_categorical": False}],
                "target": TARGET, "fault_map": {str(k): v for k, v in FAULTS.items()},
                "h_horizon_ticks": H, "dew_spec_C": DEW_SPEC,
                "det_threshold": b.det_threshold,
                "schema": "sgrn/lib/gateway/simulations/gas_processing/schema.scl"}
    man_p.write_text(json.dumps(manifest, indent=2))
    (out_dir / "model_meta.json").write_text(json.dumps({
        "champion_detector": f"PCA thr={b.det_threshold:.4f}",
        "champion_diagnoser": "RandomForestClassifier(n=200)",
        "champion_forecaster": f"RandomForestRegressor H={H} (tournament vs Ridge)",
        "metric_det_rate": metrics["det_rate"], "metric_far": metrics["far"],
        "metric_acc": metrics["acc"], "metric_known_acc": metrics["known_acc"],
        "metric_rmse_dew_ahead": metrics["rmse"], "metric_r2_dew_ahead": metrics["r2"],
        "metric_coverage": metrics["coverage"]}, indent=2))
    return csv_p, man_p


def writeReport(metrics: dict, robust: dict, out_dir: Path, n_train: int, n_test: int):
    L = ["# Gas dehydration trustworthy-AI baseline — run report", "",
         f"Train runs: {n_train} ticks (faults {TRAIN_FAULTS}) | Test ticks: {n_test} (incl. unseen 6, 7).",
         f"Forecast horizon H={H} ticks on measured dew point (spec {DEW_SPEC}C).", "",
         "## Plant reaction to injected faults (test runs)",
         "| fault | scenario | recall | fcst RMSE C | ESD% | breakthrough% | peak dew C | detect delay |",
         "|---|---|---|---|---|---|---|---|"]
    for f, r in metrics["reaction"].items():
        ttd = "n/a (normal)" if f == 0 else f"{r['ttd_med']:.0f} (miss {r['ttd_miss']})"
        L.append(f"| {f} | {FAULTS[f]} | {r['recall']:.2f} | {r['rmse_f']:.2f} | {r['esd_rate']:.0%} | "
                 f"{r['breakthrough_rate']:.0%} | {r['peak_dew']:.1f} | {ttd} |")
    L += ["",
          "## Monitor / diagnose / predict",
          f"- Detector (PCA on normal only): recall {metrics['det_rate']:.3f}, FAR {metrics['far']:.3f}.",
          f"- Diagnoser (RF): acc {metrics['acc']:.3f} overall, {metrics['known_acc']:.3f} known.",
          f"- Forecaster dew+{H} (RF champion, tournament vs Ridge): RMSE {metrics['rmse']:.2f} C, R2 {metrics['r2']:.3f}, "
          f"coverage {metrics['coverage']:.3f}.",
          f"- Doer-checker: blocked {metrics['blocked_rate']:.2%} incl. "
          f"{metrics['false_trip_vetos']} false-trip vetos (analyzer-bias fault).", "",
          "## Robustness (detection recall / forecast RMSE)"]
    for k, v in robust.items():
        L.append(f"- {k}: det {v[0]:.3f}, RMSE {v[1]:.2f} C")
    L += ["",
          "## Confusion matrix (rows=true, cols=pred)",
          "```", str(metrics["cm"]), "```",
          "",
          "## How the predictor works (for operators)",
          f"- Every tick the forecaster reads {len(FEATURE_NAMES)} twin tags and predicts the measured "
          f"dew point {H} ticks ahead, with a ±2σ band. If the band upper edge crosses the ESD line "
          f"(spec+5C) the doer proposes a trip; the checker independently re-derives dew from bed "
          "moisture (immune to analyzer bias) and vetos trips the beds do not support.",
          "- The detector watches PCA reconstruction error calibrated on an independent normal run; "
          "the diagnoser names the fault and the explanation lists the tags that deviate most.",
          "",
          "## Mapping to trustworthy autonomous operations",
          "- Monitor/diagnose/act with pre-execution verification (doer-checker) and sensor-fault-aware "
          "cross-checks — the analyzer-bias scenario is the canonical false-trip trap.",
          "- Physics + twin: surrogate shares sequencer timings, PID behaviour, tags and ESD rules with "
          "the SGRN SCL schema; swap in historian data without changing the pipeline."]
    (out_dir / "report.md").write_text("\n".join(L) + "\n")


def pushToGateway(url: str, dew_pred: float, conf: float, esd: bool):
    Gateway, _, _, _, _ = bindings()
    Gateway(url).writeField("OutletQuality/dew_point", float(dew_pred))


# --------------------------------------------------------------------------
# 6. Twin mode: Gateway A (plant truth) + Gateway B (prediction mirror),
#    dashboards side by side
# --------------------------------------------------------------------------

TWIN_SCHEMA = BASE_DIR / "sgrn" / "lib" / "gateway" / "simulations" / "gas_processing" / "schema.scl"

# Flat surrogate feature -> twin tag mapping for Gateway A (ground truth).
# Fields with no physical tag (e.g. worst_moisture) are derived on the
# dashboard from the per-tower values instead.
_TOWER_N = 3


def findGatewayBinary() -> Path:
    for c in [BASE_DIR / ".dist" / "linux-static-release" / "gateway",
              BASE_DIR / ".prefix" / "bin" / "gateway",
              BASE_DIR / ".build" / "linux-static-release" / "gateway"]:
        if c.exists():
            return c
    raise FileNotFoundError("gateway binary not found — build first (cmake --build ... --target install)")


def bindings():
    """Import the repo's own Python bindings (sgrn.gateway / sgrn.telemetry)."""
    import sys as _sys
    _p = str(BASE_DIR / "sgrn" / "python")
    if _p not in _sys.path:
        _sys.path.insert(0, _p)
    from sgrn.gateway import Gateway, GatewayHTTPError
    from sgrn.telemetry import GatewayTelemetry, TelemetryEngine, NumpyHistory
    return Gateway, GatewayHTTPError, GatewayTelemetry, TelemetryEngine, NumpyHistory


def twinPost(base_url: str, path: str, value, retries: int = 4) -> int:
    """Write one leaf/merge to the twin via the sgrn.gateway binding."""
    import time as _time
    Gateway, GatewayHTTPError, _, _, _ = bindings()
    gw = Gateway(base_url)
    for attempt in range(retries):
        try:
            is_merge = isinstance(value, dict)
            result = gw.writeFields(path, value) if is_merge else gw.writeField(path, value)
            written = result.fields_written or (0 if is_merge else 1)
            if written < 1:
                raise RuntimeError(
                    f"POST /data/{path} returned fields_written={result.fields_written}"
                )
            return written
        except GatewayHTTPError as e:
            if getattr(e, "status", 0) == 429 and attempt + 1 < retries:
                delay = min(2 ** attempt, 8)
                print(f"[gateway] HTTP 429 from {base_url}/data/{path}; "
                      f"retry {attempt + 2}/{retries} in {delay}s")
                _time.sleep(delay)
                continue
            raise


def twinPostBatch(base_url: str, items, retries: int = 4) -> None:
    """Atomic multi-field twin write via the sgrn.gateway binding."""
    import time as _time
    Gateway, GatewayHTTPError, _, _, _ = bindings()
    gw = Gateway(base_url)
    for attempt in range(retries):
        try:
            gw.memoryBatchWrite(items)
            return
        except GatewayHTTPError as e:
            if getattr(e, "status", 0) == 429 and attempt + 1 < retries:
                _time.sleep(1.0)
                continue
            raise


def twinWaitReady(base_url: str, timeout_s: float = 25.0) -> None:
    Gateway, _, _, _, _ = bindings()
    gw = Gateway(base_url)
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            reg = gw.registry(t_headers_only=True)
            if reg.dbs:
                return
        except Exception:
            time.sleep(0.5)
    raise RuntimeError(f"gateway at {base_url} not ready after {timeout_s}s")


def twinConfigs(workdir: Path, port_a: int, port_b: int) -> tuple[Path, Path]:
    import shutil
    workdir.mkdir(parents=True, exist_ok=True)
    # Fresh twin state per demo run (otherwise historian + live tags accumulate
    # across runs with different faults/seeds).
    for d in (workdir / "historian_A", workdir / "state_B"):
        shutil.rmtree(d, ignore_errors=True)
    namespaces = ["InletSeparation", "AdsorberTowers", "RegenSystem",
                  "OutletQuality", "SafetySystems", "Utilities"]
    cfg_a = {
        "schema": str(TWIN_SCHEMA),
        "state_dir": str(workdir / "historian_A"),
        "security_policy": "permissive",
        "southbound": {"s7": {"ip": "127.0.0.1", "port": 10102}},
        "northbound": {"http": {"ip": "127.0.0.1", "port": port_a,
                                "rate_limit_max_requests": 60000,
                                "rate_limit_window_s": 60},
                       "websocket": {"ip": "127.0.0.1", "port": port_a},
                       "opcua": {"ip": "127.0.0.1", "port": 4840}},
        "persistence": {"enabled": True, "mode": "changes_with_timestamp",
                        "namespaces": namespaces, "max_events": 500,
                        "batch_interval_s": 2, "zstd_level": 5},
    }
    cfg_b = {  # prediction mirror: northbound only, no historian
        "schema": str(TWIN_SCHEMA),
        "state_dir": str(workdir / "state_B"),
        "security_policy": "permissive",
        "northbound": {"http": {"ip": "127.0.0.1", "port": port_b,
                                "rate_limit_max_requests": 60000,
                                "rate_limit_window_s": 60},
                       "websocket": {"ip": "127.0.0.1", "port": port_b}},
        "persistence": {"enabled": False},
    }
    pa, pb = workdir / "twin_a.json", workdir / "twin_b.json"
    pa.write_text(json.dumps(cfg_a, indent=2))
    pb.write_text(json.dumps(cfg_b, indent=2))
    return pa, pb


def payloadsForA(row: dict) -> tuple[dict, list[tuple[str, object]]]:
    """Merge-write payloads per DB + scalar tower element writes for Gateway A.

    Struct-array element writes (towers[i].*) now land directly thanks to the
    gateway-side read-modify-write fix — one request per leaf, no workaround.
    """
    merges = {
        "InletSeparation": {
            "feed_pressure": row["InletSeparation.feed_pressure"],
            "feed_temp": row["InletSeparation.feed_temp"],
            "feed_flow": {"rate": row["InletSeparation.feed_flow"]},
            "coalescer_dp": row["InletSeparation.coalescer_dp"],
            "scrubber_level_pct": row["InletSeparation.scrubber_level"],
        },
        "AdsorberTowers": {
            "lead_lag_index": int(row["AdsorberTowers.lead_lag_index"]),
            "system_inlet_press": row["AdsorberTowers.system_inlet_press"],
            "system_outlet_press": row["AdsorberTowers.system_outlet_press"],
        },
        "RegenSystem": {
            "heater_outlet_temp": row["RegenSystem.heater_outlet_temp"],
            "heater_duty_pct": row["RegenSystem.heater_duty"],
            "heater_flame_on": bool(row["RegenSystem.heater_flame"]),
            "cooler_outlet_temp": row["RegenSystem.cooler_outlet_temp"],
            "blower_flow": {"rate": row["RegenSystem.blower_flow"]},
            "regen_water_removed_l": row["RegenSystem.regen_water_removed"],
        },
        "OutletQuality": {
            "dew_point": row["OutletQuality.dew_point"],
            "sales_gas_flow": {"rate": row["OutletQuality.sales_flow"]},
        },
        "SafetySystems": {"esd_signal": bool(row["SafetySystems.esd_signal"])},
        "Utilities": {"fuel_gas_press": row["Utilities.fuel_gas_press"]},
    }
    tower_scalars = []
    for i in range(_TOWER_N):
        tower_scalars.append((f"AdsorberTowers/towers/{i}/moisture_loading",
                              row[f"AdsorberTowers.moisture_{i}"]))
        tower_scalars.append((f"AdsorberTowers/towers/{i}/cycle_state",
                              int(row[f"cycle_{i}"])))
        tower_scalars.append((f"AdsorberTowers/towers/{i}/bed_dp",
                              row[f"bed_dp_{i}"]))
    return merges, tower_scalars


def payloadsForB(dew_fc: float, anomaly: bool, fault_pred: int, trip: bool,
                   dew_std: float = 0.0) -> dict:
    """Prediction mirror writes to explicitly model-owned fields."""
    return {"SafetySystems": {"model_trip_advice": bool(trip),
                              "model_anomaly": bool(anomaly),
                              "model_fault_code": int(fault_pred)},
            "OutletQuality": {"dew_point_forecast": float(dew_fc),
                              "dew_point_forecast_std": float(dew_std)}}


def openSideBySide(url_a: str, url_b: str) -> list:
    """Pop two browser windows, left/right halves. Best-effort, no crash."""
    import shutil
    import subprocess
    from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit
    procs = []

    def wmIds():
        try:
            out = subprocess.run(["wmctrl", "-l"], capture_output=True, text=True,
                                 timeout=5).stdout
            return {l.split()[0] for l in out.splitlines() if l.split()}
        except Exception:
            return set()

    def place(wid_list, x: int, w: int, y: int = 26, h: int = 1054):
        import subprocess as _sp
        for wid in wid_list:
            try:
                _sp.run(["wmctrl", "-i", "-r", wid, "-e", f"0,{x},{y},{w},{h}"],
                        timeout=5, stdout=_sp.DEVNULL, stderr=_sp.DEVNULL)
            except Exception:
                pass

    named_urls = []
    for url, name in ((url_a, "Recorded history"), (url_b, "Model predictions")):
        parts = urlsplit(url)
        query = dict(parse_qsl(parts.query, keep_blank_values=True))
        query["gatewayName"] = name
        named_urls.append(urlunsplit((parts.scheme, parts.netloc, parts.path,
                                      urlencode(query), parts.fragment)))
    url_a, url_b = named_urls

    try:
        out = subprocess.run(["xdpyinfo"], capture_output=True, text=True, timeout=5).stdout
        import re
        m = re.search(r"dimensions:\s+(\d+)x(\d+)", out)
        W, Hh = (int(m.group(1)), int(m.group(2))) if m else (1920, 1080)
    except Exception:
        W, Hh = 1920, 1080
    half, gh = W // 2, Hh - 26  # leave room for the top panel
    chrom = shutil.which("chromium") or shutil.which("chromium-browser") \
        or shutil.which("google-chrome") or shutil.which("google-chrome-stable")
    try:
        if chrom and shutil.which("wmctrl"):
            # launch one at a time and pin each new window by id diff
            ids = []
            for i, (url, x) in enumerate([(url_a, 0), (url_b, half)]):
                before = wmIds()
                profile = f"/tmp/sgrn-twin-{'left' if i == 0 else 'right'}"
                shutil.rmtree(profile, ignore_errors=True)
                procs.append(subprocess.Popen(
                    [chrom, f"--app={url}", f"--window-position={x},0",
                     f"--window-size={half},{Hh}", f"--user-data-dir={profile}",
                     "--no-first-run"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                import time as _t
                new = []
                for _ in range(20):
                    _t.sleep(0.5)
                    new = list(wmIds() - before)
                    if new:
                        break
                ids.append((new, x))
            for new, x in ids:
                place(new, x, half, 26, gh)
            return procs
        if chrom:
            for i, (url, x) in enumerate([(url_a, 0), (url_b, half)]):
                profile = f"/tmp/sgrn-twin-{'left' if i == 0 else 'right'}"
                shutil.rmtree(profile, ignore_errors=True)
                procs.append(subprocess.Popen(
                    [chrom, f"--app={url}", f"--window-position={x},0",
                     f"--window-size={half},{Hh}", f"--user-data-dir={profile}",
                     "--no-first-run"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            return procs
        if shutil.which("firefox"):
            procs.append(subprocess.Popen(["firefox", "--new-window", url_a, url_b],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            print("      (firefox can't tile windows itself — snap them left/right manually)")
            return procs
        import webbrowser
        webbrowser.open(url_a)
        webbrowser.open(url_b)
        print("      (no chromium/firefox found — opened default browser tabs instead)")
    except Exception as e:
        print(f"      browser pop failed ({e}) — open {url_a} and {url_b} manually")
    return procs


def loadOrTrainBundle(args, out_dir: Path) -> Bundle:
    import pickle
    cache = out_dir / "bundle.pkl"
    if cache.exists() and not args.retrain:
        with open(cache, "rb") as f:
            print("      loaded cached predictor (use --retrain to refit)")
            return pickle.load(f)
    print(f"      training predictor ({args.runs} runs/fault — cached afterwards) ...")
    df_train = generateGas(args.runs, TRAIN_FAULTS, seed=args.seed)
    df_cal = generateGas(2, [0], seed=args.seed + 999)
    b = trainBundle(df_train, df_cal)
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(cache, "wb") as f:
        pickle.dump(b, f)
    return b


TWIN_SUBS_A = ["InletSeparation", "AdsorberTowers", "RegenSystem",
               "OutletQuality", "SafetySystems", "Utilities"]
TWIN_SUBS_B = ["OutletQuality", "SafetySystems"]

# Model feature -> TelemetryEngine flat key ("<Db>-<path>", arrays fan out to
# "[i]" segments, e.g. "AdsorberTowers-towers/[1]/moisture_loading").
# worst_moisture / regen_state / tower fields have no single physical tag:
# towers come from the authoritative structured record (readDbArray).
TWIN_KEYS = {
    "InletSeparation.feed_pressure": "InletSeparation-feed_pressure",
    "InletSeparation.feed_temp": "InletSeparation-feed_temp",
    "InletSeparation.feed_flow": "InletSeparation-feed_flow/rate",
    "InletSeparation.coalescer_dp": "InletSeparation-coalescer_dp",
    "InletSeparation.scrubber_level": "InletSeparation-scrubber_level_pct",
    "AdsorberTowers.moisture_0": "AdsorberTowers-towers/[0]/moisture_loading",
    "AdsorberTowers.moisture_1": "AdsorberTowers-towers/[1]/moisture_loading",
    "AdsorberTowers.moisture_2": "AdsorberTowers-towers/[2]/moisture_loading",
    "AdsorberTowers.lead_lag_index": "AdsorberTowers-lead_lag_index",
    "AdsorberTowers.system_inlet_press": "AdsorberTowers-system_inlet_press",
    "AdsorberTowers.system_outlet_press": "AdsorberTowers-system_outlet_press",
    "RegenSystem.heater_outlet_temp": "RegenSystem-heater_outlet_temp",
    "RegenSystem.heater_duty": "RegenSystem-heater_duty_pct",
    "RegenSystem.heater_flame": "RegenSystem-heater_flame_on",
    "RegenSystem.cooler_outlet_temp": "RegenSystem-cooler_outlet_temp",
    "RegenSystem.blower_flow": "RegenSystem-blower_flow/rate",
    "RegenSystem.regen_water_removed": "RegenSystem-regen_water_removed_l",
    "OutletQuality.dew_point": "OutletQuality-dew_point",
    "OutletQuality.sales_flow": "OutletQuality-sales_gas_flow/rate",
    "SafetySystems.esd_signal": "SafetySystems-esd_signal",
    "Utilities.fuel_gas_press": "Utilities-fuel_gas_press",
}


def nestFlatLeaves(items, id_to_dotted):
    """Rebuild the nested {Db: {...}} snapshot TelemetryEngine expects from
    flat dictionary-mode leaves {"<id>": value|{value,ts}}.

    Dotted paths look like "AdsorberTowers.towers[0].moisture_loading" (legacy
    indexed form) or "AdsorberTowers.towers.moisture_loading" with an ARRAY
    value (one entry per element, server-side contract for array-of-struct
    leaves). Both rebuild into real lists so the engine flattens them back to
    its canonical "Db-path/[i]/leaf" keys — identical to legacy nested frames.
    """
    root: dict = {}

    def descend(node, segs):
        for s in segs[:-1]:
            if s.endswith("]") and "[" in s:
                name, idx = s[:-1].split("[")
                lst = node.setdefault(name, [])
                idx = int(idx)
                while len(lst) <= idx:
                    lst.append({})
                node = lst[idx]
            else:
                nxt = node.get(s)
                if not isinstance(nxt, dict):
                    nxt = {}
                    node[s] = nxt
                node = nxt
        return node, segs[-1]

    for idstr, entry in items:
        try:
            leaf_id = int(idstr)
        except (TypeError, ValueError):
            continue
        dotted = id_to_dotted.get(leaf_id)
        if not dotted or "." not in dotted:
            continue
        if isinstance(entry, dict) and "value" in entry:
            entry = entry.get("value")
        db, _, rest = dotted.partition(".")
        node = root.setdefault(db, {})
        if isinstance(entry, list):
            # array-of-struct leaf: one value per element
            segs = rest.split(".")
            if not segs:
                continue
            arr_name = segs[0].split("[")[0]
            sub = segs[1:]
            arr = node.setdefault(arr_name, [])
            while len(arr) < len(entry):
                arr.append({})
            for i, v in enumerate(entry):
                if not sub:
                    continue
                target, last = descend(arr[i], sub)
                target[last] = v
        else:
            target, last = descend(node, rest.split("."))
            target[last] = entry
    return root


class TwinStream:
    """A subscribed view of one gateway twin, built only from repo bindings:

    GatewayTelemetry (WS subscribe + auto-reconnect) -> TelemetryEngine
    (flatten + batch) -> latest{key: value} + NumpyHistory ring buffers.

    Wire detail (mirrors web-gateway worker.ts): after connect the server
    pushes a nested seed snapshot plus a {"type":"dictionary"} leaf table.
    We answer with setDictionaryMode, after which deltas arrive as flat
    {"<leaf_id>": value} frames that are rebuilt into nested snapshots, so
    the engine emits the exact same canonical keys for seed and deltas.

    Every read below comes from the twin over the websocket — never from the
    local surrogate state.
    """

    def __init__(self, base_url: str):
        _, _, GatewayTelemetry, TelemetryEngine, NumpyHistory = bindings()
        ws_url = (base_url.rstrip("/")
                  .replace("https://", "wss://").replace("http://", "ws://") + "/ws")
        self.base_url = base_url
        self.engine = TelemetryEngine(t_flush_interval_ms=32.0)
        self.history = NumpyHistory(t_capacity=512)
        self.engine.history = self.history
        self.latest: dict = {}
        self.flushes = 0
        self.id_to_dotted: dict = {}
        self.dict_mode = False

        def onBatch(batch):
            for k, u in batch.items():
                self.latest[k] = u.get("value")
            self.flushes += 1

        self.engine.on_batch = onBatch
        self.tel = GatewayTelemetry(ws_url, t_on_message=self.routeFrame)

    def routeFrame(self, data) -> None:
        if not isinstance(data, dict) or not data:
            return
        if data.get("type") == "dictionary":
            leaves = data.get("leaves") or []
            self.id_to_dotted = {int(l["id"]): l["path"] for l in leaves
                                 if isinstance(l, dict) and "id" in l and "path" in l}
            if self.id_to_dotted:
                self.dict_mode = True
                self.tel.sendRaw({"command": "setDictionaryMode", "enabled": True})
            return
        if self.dict_mode and all(k.isdigit() for k in data):
            nested = nestFlatLeaves(data.items(), self.id_to_dotted)
            if nested:
                self.engine.handleFrame(nested)
            return
        self.engine.handleFrame(data)

    async def start(self, subs: list) -> None:
        import asyncio as _a
        for s in subs:
            self.tel.subscribe(s)
            self.engine.subscribe(s)
        self.tel.start()
        for _ in range(100):
            if self.tel.connected:
                break
            await _a.sleep(0.1)
        if not self.tel.connected:
            raise RuntimeError("websocket connect failed")
        for _ in range(100):
            if self.flushes:
                break
            await _a.sleep(0.1)
        if not self.flushes:
            raise RuntimeError("no telemetry frames received (seed snapshot missing)")

    async def stop(self) -> None:
        await self.tel.stop()

    def value(self, key: str, default=None):
        return self.latest.get(key, default)

    async def waitValue(self, key: str, want: float, tol: float = 2e-3,
                         timeout: float = 1.0) -> bool:
        """Wait until the twin echoes the value just written (float32-safe)."""
        import asyncio as _a
        import time as _t
        t0 = _t.time()
        while _t.time() - t0 < timeout:
            v = self.latest.get(key)
            try:
                if v is not None and abs(float(v) - float(want)) <= tol:
                    return True
            except (TypeError, ValueError):
                if v == want:
                    return True
            await _a.sleep(0.02)
        return False


def twinFeatures(stream: TwinStream, row: dict) -> tuple:
    """Build the predictor input from the subscribed twin push channel.

    The WS leaf dictionary now carries one value per array element, so tower
    fields arrive like every other tag (flat arrays fan out to towers/[i]/*
    keys in the translator). worst_moisture / regen_state are derived from
    twin tags. Returns (vector, n_fallbacks) — fallbacks mean a tag never
    arrived over the socket (should be ~0 after the seed frame).
    """
    import numpy as np
    fb = 0

    def g(key: str, fallback):
        nonlocal fb
        v = stream.value(key)
        if v is None:
            fb += 1
            return float(fallback)
        try:
            return float(v)
        except (TypeError, ValueError):
            fb += 1
            return float(fallback)

    moist = [g(f"AdsorberTowers-towers/[{i}]/moisture_loading",
               row[f"AdsorberTowers.moisture_{i}"]) for i in range(3)]
    worst = max(moist)
    lead = int(g(TWIN_KEYS["AdsorberTowers.lead_lag_index"],
                 row["AdsorberTowers.lead_lag_index"]))
    if 0 <= lead <= 2:
        rstate = g(f"AdsorberTowers-towers/[{lead}]/cycle_state", row[f"cycle_{lead}"])
    else:
        rstate = float(row["AdsorberTowers.regen_state"])
        fb += 1

    vec = []
    for name in FEATURE_NAMES:
        if name == "AdsorberTowers.worst_moisture":
            vec.append(worst)
        elif name == "AdsorberTowers.regen_state":
            vec.append(rstate)
        elif name.startswith("AdsorberTowers.moisture_"):
            vec.append(moist[int(name.rsplit("_", 1)[1])])
        else:
            vec.append(g(TWIN_KEYS[name], row[name]))
    return np.array([vec], float), fb


def runTwinMode(args) -> None:
    import numpy as np
    import subprocess
    fault = args.fault
    if fault not in FAULTS:
        raise SystemExit(f"unknown --fault {fault}, choose from {sorted(FAULTS)}")
    out_dir = Path(args.out)
    gw_bin = findGatewayBinary()
    workdir = out_dir / "twin"
    pa, pb = twinConfigs(workdir, args.port_a, args.port_b)
    url_a, url_b = f"http://127.0.0.1:{args.port_a}", f"http://127.0.0.1:{args.port_b}"

    b = loadOrTrainBundle(args, out_dir)

    print(f"[twin] starting Gateway A (plant truth) :{args.port_a} ...")
    log_a = open(workdir / "gateway_A.log", "w")
    proc_a = subprocess.Popen([str(gw_bin), str(pa)], cwd=str(BASE_DIR),
                              stdout=log_a, stderr=subprocess.STDOUT)
    print(f"[twin] starting Gateway B (prediction mirror) :{args.port_b} ...")
    log_b = open(workdir / "gateway_B.log", "w")
    proc_b = subprocess.Popen([str(gw_bin), str(pb)], cwd=str(BASE_DIR),
                              stdout=log_b, stderr=subprocess.STDOUT)
    browsers = []
    try:
        twinWaitReady(url_a)
        twinWaitReady(url_b)
        print(f"[twin] A ready at {url_a}  |  B ready at {url_b}")
        if not args.no_browser:
            print("[twin] popping dashboards side by side (left=truth, right=prediction) ...")
            browsers = openSideBySide(url_a, url_b)

        rows = runPlant(fault, RUN_STEPS, args.seed)
        print(f"[twin] streaming F{fault} {FAULTS[fault]} @ {args.speed}/s "
              f"(fault injects at tick {T_INJ}; Ctrl+C stops, gateways shut down)")
        print("      predictor input = SUBSCRIBED twin tags (echo-gated per tick); "
              "fallbacks counted below")
        print("      tick | dew true | dew+5 fcst | worst moist | heater | ESD | flag | action -> checker")
        import asyncio as _asyncio
        _asyncio.run(streamLoop(args, url_a, url_b, b, fault, rows, workdir))
    finally:
        if args.keep_up:
            print("[twin] --keep-up: gateways left running; Ctrl+C browsers to close.")
            print(f"      A: {url_a}   B: {url_b}")
        else:
            print("[twin] shutting down gateways ...")
            for p in (proc_a, proc_b):
                if p.poll() is None:
                    p.terminate()
            for p in (proc_a, proc_b):
                try:
                    p.wait(timeout=10)
                except Exception:
                    p.kill()
            for p in browsers:
                if p.poll() is None:
                    p.terminate()
            log_a.close()
            log_b.close()
            print("[twin] shutdown complete.")


# --------------------------------------------------------------------------
# 5. CLI modes
# --------------------------------------------------------------------------

async def streamLoop(args, url_a: str, url_b: str, b: Bundle, fault: int,
                       rows: list, workdir: Path) -> None:
    """Tick loop: write truth -> echo-gate on subscribed tags -> predict from
    twin -> checker -> write mirror. Displayed numbers are twin-echoed."""
    import asyncio as _a
    import time as _t

    stream_a = TwinStream(url_a)
    stream_b = TwinStream(url_b)
    print("[twin] subscribing to Gateway A (truth) ...")
    await stream_a.start(TWIN_SUBS_A)
    print(f"[twin] A subscribed ({len(stream_a.latest)} tags from seed snapshot)")
    print("[twin] subscribing to Gateway B (mirror) ...")
    await stream_b.start(TWIN_SUBS_B)
    print(f"[twin] B subscribed ({len(stream_b.latest)} tags from seed snapshot)")

    esd_prev = False
    fb_total = fb_keys = lag_ticks = 0
    t_end = _t.time()
    try:
        for row in rows:
            t0 = _t.time()
            tick = int(row["tick"])

            merges, scalars = payloadsForA(row)
            for db, obj in merges.items():
                twinPost(url_a, db, obj)
            for path, val in scalars:
                twinPost(url_a, path, val)

            # echo-gate on the subscribed dew push, then build the predictor
            # input from subscribed twin tags (towers included via WS arrays).
            echoed = await stream_a.waitValue(
                "OutletQuality-dew_point", row["OutletQuality.dew_point"])
            lag_ticks += int(not echoed)
            X, nfb = twinFeatures(stream_a, row)
            fb_total += nfb
            fb_keys += 1

            o = predictWithUq(b, X)
            anomaly = bool(o["recon"][0] > b.det_threshold)
            fp = int(o["fault"][0])
            heat_tw = float(stream_a.value(
                "RegenSystem.heater_outlet_temp", row["RegenSystem.heater_outlet_temp"]))
            press_tw = float(stream_a.value(
                "InletSeparation.feed_pressure", row["InletSeparation.feed_pressure"]))
            # worst moisture from the authoritative tower record (X), NOT from
            # WS keys: dictionary deltas carry array element [0] only.
            moist_tw = float(X[0][FEATURE_NAMES.index("AdsorberTowers.worst_moisture")])
            a = doerPolicy(fp, float(o["dew_mean"][0]), heat_tw, press_tw)
            ok, why = checker(a, float(o["dew_mean"][0]), float(o["dew_std"][0]),
                              heat_tw, press_tw, float(o["conf"][0]), moist_tw)
            trip = (a == 3) and ok  # checker-gated trip advice on mirror B
            if tick == T_INJ:
                print(f"\n>>> FAULT INJECTED @ tick {tick}: F{fault} {FAULTS[fault]} "
                      f"<<<\n>>> LEFT shows the plant reacting, RIGHT shows the forecast+advice <<<\n")
            esd_now = bool(stream_a.value(
                "SafetySystems-esd_signal", row["SafetySystems.esd_signal"]))
            if esd_now and not esd_prev:
                print(f"\n!!! PLANT ESD TRIPPED @ tick {tick} (LEFT dashboard) !!!\n")
            esd_prev = esd_now
            if not ok and a == 3:
                print(f"      [checker] BLOCKED false trip @ {tick}: {why}")

            for db, obj in payloadsForB(float(o["dew_mean"][0]), anomaly, fp, trip,
                                         float(o["dew_std"][0])).items():
                twinPost(url_b, db, obj)

            dew_tw = float(stream_a.value(
                "OutletQuality-dew_point", row["OutletQuality.dew_point"]))
            moist_tw = float(X[0][FEATURE_NAMES.index("AdsorberTowers.worst_moisture")])
            flag = "!!" if anomaly else "  "
            print(f"      t={tick:3d}{flag} dew={dew_tw:7.1f}C "
                  f"fc+{H}={float(o['dew_mean'][0]):7.1f}±{float(o['dew_std'][0]):4.1f} "
                  f"moist={moist_tw:5.1f}% "
                  f"heat={heat_tw:6.0f}C ESD={int(esd_now)} "
                  f"F{fp} {ACTIONS[a]:18s} -> {'ALLOW' if ok else 'BLOCK'}")
            t_end = _t.time()
            await _a.sleep(max(0.0, 1.0 / args.speed - (t_end - t0)))

        print("\n[twin] run finished.")
        print(f"      subscribed coverage: {fb_total} fallbacks / "
              f"{fb_keys * len(FEATURE_NAMES)} feature reads "
              f"({100.0 * (1 - fb_total / max(1, fb_keys * len(FEATURE_NAMES))):.2f}% from twin)")
        print(f"      echo-gate timeouts: {lag_ticks} ticks "
              f"(fallback: surrogate row for that tick)")
        print(f"      historian archives: {workdir / 'historian_A'} "
              f"(extract with: sgrn_dataset -i {workdir / 'historian_A'} "
              f"-s {TWIN_SCHEMA} -o dataset.csv -m manifest.json)")
    finally:
        await stream_a.stop()
        await stream_b.stop()


def sparkline(vals, width=50):
    import numpy as np
    v = np.asarray(vals, float)
    lo, hi = v.min(), v.max()
    bars = " .:-=+*#%@"
    if hi - lo < 1e-9:
        return bars[0] * width
    idx = ((v - lo) / (hi - lo) * (len(bars) - 1)).astype(int)
    step = max(1, len(idx) // width)
    return "".join(bars[i] for i in idx[::step])


def main():
    ap = argparse.ArgumentParser(description="Gas dehydration trustworthy-AI experiment wired to SGRN")
    ap.add_argument("--mode", choices=["full", "train", "demo", "twin"], default="full")
    ap.add_argument("--runs", type=int, default=6, help="runs per fault (train)")
    ap.add_argument("--test-runs", type=int, default=3)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--out", type=str, default=str(OUT_DEFAULT))
    ap.add_argument("--push-gateway", type=str, default="")
    ap.add_argument("--no-plot", action="store_true")
    # twin mode: dual gateways + side-by-side dashboards
    ap.add_argument("--fault", type=int, default=1, help="fault id to inject in twin mode")
    ap.add_argument("--speed", type=float, default=8.0, help="stream ticks per second in twin mode")
    ap.add_argument("--port-a", type=int, default=8080, help="Gateway A (truth) HTTP port")
    ap.add_argument("--port-b", type=int, default=8082, help="Gateway B (mirror) HTTP port")
    ap.add_argument("--no-browser", action="store_true", help="don't pop dashboards")
    ap.add_argument("--keep-up", action="store_true", help="leave gateways running at the end")
    ap.add_argument("--retrain", action="store_true", help="ignore cached predictor, refit")
    args = ap.parse_args()

    try:
        import numpy  # noqa
        import pandas  # noqa
        import sklearn  # noqa
    except ImportError as e:
        print(f"Missing dependency: {e}. Run inside the SGRN env:  micromamba run -n SGRN python demos/gas_processing.py")
        sys.exit(1)
    import warnings
    warnings.filterwarnings("ignore", message=".*ill-conditioned.*")

    if args.mode == "twin":
        print("=" * 64 + "\n SGRN x Gas Dehydration — dual-gateway twin demo\n" + "=" * 64)
        runTwinMode(args)
        return

    out_dir = Path(args.out)
    print("=" * 64 + "\n SGRN x Gas Dehydration — fault-injection experiment + predictor\n" + "=" * 64)
    print("[1/5] Simulating 3-tower plant (fault injected at tick "
          f"{T_INJ}/{RUN_STEPS}) ...")
    df_train = generateGas(args.runs, TRAIN_FAULTS, seed=args.seed)
    df_test = generateGas(args.test_runs, list(FAULTS), seed=args.seed + 1)
    df_cal = generateGas(2, [0], seed=args.seed + 999)
    print(f"      train {df_train.shape} | test {df_test.shape} (6,7 unseen) | cal {len(df_cal)}")

    print("[2/5] Training detector + diagnoser + dew forecaster ...")
    b = trainBundle(df_train, df_cal)

    print("[3/5] How the plant reacted (test runs):")
    m = evaluate(b, df_test)
    for f, r in m["reaction"].items():
        ttd = "n/a " if f == 0 else f"{r['ttd_med']:.0f}tk"
        print(f"      F{f} {FAULTS[f][:42]:42s} rec {r['recall']:.2f} rmse {r['rmse_f']:5.2f}C "
              f"ESD {r['esd_rate']:.0%}  breakthrough {r['breakthrough_rate']:.0%}  "
              f"peak dew {r['peak_dew']:6.1f}C  detect@{ttd}")
    print(f"      detector recall {m['det_rate']:.3f} FAR {m['far']:.3f} | "
          f"diagnoser acc {m['acc']:.3f} (known {m['known_acc']:.3f})")
    print(f"      dew+{H} forecast RMSE {m['rmse']:.2f} C R2 {m['r2']:.3f} coverage {m['coverage']:.3f}")
    print(f"      checker blocked {m['blocked_rate']:.2%} ({m['false_trip_vetos']} false-trip vetos)")
    print("      confusion matrix (rows=true):\n", m["cm"])

    print("[4/5] Robustness ...")
    rob = robustnessTests(b, df_test)
    for k, v in rob.items():
        print(f"      {k:22s} det {v[0]:.3f} RMSE {v[1]:.2f} C")

    # explanation on first post-injection flag of the heater fault showcase
    import numpy as np
    X = df_test[FEATURE_NAMES].to_numpy(float)
    show = df_test[(df_test["fault"] == 1) & (df_test["post"] == 1)].iloc[:60]
    o = predictWithUq(b, show[FEATURE_NAMES].to_numpy(float))
    i = int(np.argmax(o["recon"] > b.det_threshold))
    print(f"[explain] heater-fault showcase, tick {show['tick'].iloc[i]}, "
          f"pred F{int(o['fault'][i])} conf {o['conf'][i]:.2f}, "
          f"dew+{H} {o['dew_mean'][i]:.1f}±{o['dew_std'][i]:.1f}C:")
    for name, s in explainSample(b, show[FEATURE_NAMES].to_numpy(float)[i]):
        print(f"      {name:38s} {s:+.3f}")
    r = show.iloc[i]
    a = doerPolicy(int(o["fault"][i]), float(o["dew_mean"][i]),
                    float(r["RegenSystem.heater_outlet_temp"]),
                    float(r["InletSeparation.feed_pressure"]))
    ok, why = checker(a, float(o["dew_mean"][i]), float(o["dew_std"][i]),
                      float(r["RegenSystem.heater_outlet_temp"]),
                      float(r["InletSeparation.feed_pressure"]),
                      float(o["conf"][i]), float(r["AdsorberTowers.worst_moisture"]))
    print(f"      doer proposes '{ACTIONS[a]}' -> checker {'ALLOW' if ok else 'BLOCK'} ({why})")

    print("[5/5] Exporting SGRN artifacts ...")
    import pandas as pd
    df_all = pd.concat([df_train.assign(split="train"), df_test.assign(split="test")],
                       ignore_index=True).drop(columns=["split"])
    csv_p, man_p = exportSgrn(df_all, b, m, out_dir)
    writeReport(m, rob, out_dir, len(df_train), len(df_test))
    print(f"      {csv_p}\n      {man_p}\n      {out_dir/'model_meta.json'}\n      {out_dir/'report.md'}")
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
            pushToGateway(args.push_gateway, float(o["dew_mean"][i]), float(o["conf"][i]), False)
            print(f"      pushed forecast to {args.push_gateway}/data/OutletQuality")
        except Exception as e:
            print(f"      gateway push failed: {e}")

    if args.mode == "demo":
        print("\n[live replay: heater flame-out run — tick | dew meas | dew+5 pred | worst moist | heater | ESD | flag]")
        demo_run = df_test[df_test["run"] == "f1r0"].reset_index(drop=True)
        Xd = demo_run[FEATURE_NAMES].to_numpy(float)
        od = predictWithUq(b, Xd)
        for k in range(0, len(demo_run), 4):
            flag = "!!" if od["recon"][k] > b.det_threshold else "  "
            print(f"  t={int(demo_run['tick'][k]):3d}{flag} dew={demo_run['OutletQuality.dew_point'][k]:7.1f}C "
                  f"pred+{H}={od['dew_mean'][k]:7.1f}±{od['dew_std'][k]:4.1f} "
                  f"moist={demo_run['AdsorberTowers.worst_moisture'][k]:5.1f}% "
                  f"heat={demo_run['RegenSystem.heater_outlet_temp'][k]:6.0f}C "
                  f"ESD={int(demo_run['SafetySystems.esd_signal'][k])}")
            time.sleep(0.05)
        print("\n[reaction trace] measured dew around injection:")
        seg = demo_run[(demo_run["tick"] >= T_INJ - 10) & (demo_run["tick"] <= T_INJ + 40)]
        print("  " + sparkline(seg["OutletQuality.dew_point"].to_numpy()))
        print(f"  (tick {T_INJ - 10}..{T_INJ + 40}, range "
              f"{seg['OutletQuality.dew_point'].min():.1f}..{seg['OutletQuality.dew_point'].max():.1f} C)")

    if not args.no_plot:
        try:
            import matplotlib.pyplot as plt
            demo_run = df_test[df_test["run"] == "f1r0"].reset_index(drop=True)
            Xd = demo_run[FEATURE_NAMES].to_numpy(float)
            od = predictWithUq(b, Xd)
            fig, ax = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
            ax[0].plot(demo_run["tick"], demo_run["dew_true"], label="true dew", color="navy")
            ax[0].plot(demo_run["tick"], demo_run["OutletQuality.dew_point"], label="measured",
                       color="crimson", alpha=0.7)
            ax[0].axhline(DEW_SPEC, color="k", ls="--", label="spec")
            ax[0].axvline(T_INJ, color="k", ls=":", label="fault injected")
            ax[0].set_ylabel("dew C"); ax[0].legend(loc="upper left"); ax[0].set_title("F1 heater flame-out: reaction")
            ax[1].plot(demo_run["tick"], demo_run["RegenSystem.heater_outlet_temp"], label="heater T")
            ax[1].plot(demo_run["tick"], demo_run["AdsorberTowers.worst_moisture"], label="worst moisture %")
            ax[1].axvline(T_INJ, color="k", ls=":"); ax[1].legend(loc="upper left"); ax[1].set_ylabel("T / %")
            ax[2].plot(demo_run["tick"], od["recon"], label="detector score")
            ax[2].axhline(b.det_threshold, color="k", ls="--", label="threshold")
            ax[2].axvline(T_INJ, color="k", ls=":"); ax[2].legend(loc="upper left")
            ax[2].set_xlabel("tick"); ax[2].set_ylabel("recon error")
            fig.tight_layout(); fig.savefig(out_dir / "trajectory.png", dpi=120)
            fig2, ax2 = plt.subplots(figsize=(5, 5))
            ax2.scatter(df_test[TARGET], m["out"]["dew_mean"], s=3, alpha=0.3)
            ax2.set_xlabel("true dew+H C"); ax2.set_ylabel("predicted C"); ax2.set_title("Forecaster parity")
            fig2.tight_layout(); fig2.savefig(out_dir / "parity.png", dpi=120)
            print(f"      {out_dir/'trajectory.png'}\n      {out_dir/'parity.png'}")
        except Exception as e:
            print(f"      plot skipped ({e})")

    print("=" * 64 + "\n done. See scratch/gas_demo/report.md + trajectory.png.\n" + "=" * 64)


if __name__ == "__main__":
    main()
