#!/usr/bin/env python3
"""Train FC autoencoder on fault-free TE runs + split-conformal threshold.

SIMULATED DATA ONLY. Not a live plant, not real-plant validation.

Trains ONLY on fault-free training runs. Standardisation fitted on the
training split only and saved with the model. Split by RUN (never by
sample). Calibration uses HELD-OUT fault-free runs not used for training.

Conformal note: exchangeability does NOT strictly hold for time series
(neighbouring samples are dependent), so the coverage guarantee is
approximate. Threshold = ceil((n+1)(1-alpha))/n empirical quantile.

Reads converted jsonl.zst offline (fast). Evaluation runs online through
the gateway (see te_experiment.py) and must not read RData directly.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

import numpy as np

FEATURES = [
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
]  # twin paths TE.<name>; order matches te_rieth/schema.scl DB "TE"


def get_torch():
    import torch

    return torch


def build_model(torch, dim: int = 52):
    import torch.nn as nn

    return nn.Sequential(
        nn.Linear(dim, 32), nn.ReLU(),
        nn.Linear(32, 16), nn.ReLU(),
        nn.Linear(16, 8), nn.ReLU(),
        nn.Linear(8, 16), nn.ReLU(),
        nn.Linear(16, 32), nn.ReLU(),
        nn.Linear(32, dim),
    )


def decode_archive(path: Path):
    """Return (N,52) float32 rows in FEATURES order from one jsonl.zst."""
    import zstandard as zstd

    raw = zstd.ZstdDecompressor().decompress(path.read_bytes()).decode().splitlines()
    id2path: dict[str, str] = {}
    rows: list[list[float]] = []
    cur: dict[str, float] = {}
    for line in raw:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        t = obj.get("type")
        if t == "dictionary":
            for leaf in obj.get("leaves", []):
                id2path[str(leaf["id"])] = leaf["path"]
        elif t in ("anchor", "delta"):
            payload = obj.get("data", obj.get("changes", {}))
            for k, v in payload.items():
                cur[id2path.get(k, k)] = float(v)
            rows.append([float(cur.get(f"TE.{c}", 0.0)) for c in FEATURES])
    return np.asarray(rows, dtype=np.float32)


def load_split(indir: Path, faults: set[int], runs: set[int]):
    xs = []
    for path in sorted(indir.glob("*.jsonl.zst")):
        stem = path.stem.split(".")[0]  # te_train_f00_run001
        try:
            f = int(stem.split("_")[2][1:])
            r = int(stem.split("_")[3][3:])
        except (IndexError, ValueError):
            continue
        if f in faults and r in runs:
            xs.append(decode_archive(path))
    if not xs:
        raise RuntimeError(f"no archives match faults={faults} runs={runs} in {indir}")
    return xs


def conformal_threshold(scores: np.ndarray, alpha: float) -> float:
    n = int(scores.shape[0])
    k = int(np.ceil((n + 1) * (1.0 - alpha)))
    k = min(max(k, 1), n)
    return float(np.sort(scores)[k - 1])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--indir", required=True, help="converted jsonl.zst dir")
    ap.add_argument("--outdir", required=True, help="model output dir")
    ap.add_argument("--train-runs", type=int, nargs="*", default=[1, 2, 3, 4])
    ap.add_argument("--cal-runs", type=int, nargs="*", default=[5, 6])
    ap.add_argument("--alpha", type=float, default=0.01)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--lr", type=float, default=1e-3)
    args = ap.parse_args()

    if not 0 < args.alpha < 1:
        print("alpha must be in (0,1)", file=sys.stderr)
        return 2
    if set(args.train_runs) & set(args.cal_runs):
        print("train/cal runs must be disjoint (split by RUN)", file=sys.stderr)
        return 2

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch = get_torch()
    torch.manual_seed(args.seed)
    torch.use_deterministic_algorithms(True)

    indir = Path(args.indir)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    train_runs = load_split(indir, {0}, set(args.train_runs))
    cal_runs = load_split(indir, {0}, set(args.cal_runs))
    x_train = np.concatenate(train_runs, axis=0)
    mu = x_train.mean(axis=0).astype(np.float64)
    sd = (x_train.std(axis=0).astype(np.float64) + 1e-8)
    np.savez(out / "scaler.npz", mean=mu, std=sd)

    xt = torch.from_numpy(((x_train - mu) / sd).astype(np.float32))
    gen = torch.Generator().manual_seed(args.seed)
    loader = torch.utils.data.DataLoader(
        torch.utils.data.TensorDataset(xt), batch_size=256, shuffle=True,
        num_workers=0, generator=gen,
    )
    model = build_model(torch)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    model.train()
    for _ in range(max(1, args.epochs)):
        for (batch,) in loader:
            opt.zero_grad()
            loss = ((model(batch) - batch) ** 2).mean()
            loss.backward()
            opt.step()

    model.eval()
    with torch.no_grad():
        cal_scores = []
        for run in cal_runs:
            xc = torch.from_numpy(((run - mu) / sd).astype(np.float32))
            err = ((model(xc) - xc) ** 2).mean(dim=1).numpy()
            cal_scores.append(err)
    cal_scores = np.concatenate(cal_scores)
    threshold = conformal_threshold(cal_scores, args.alpha)

    torch.save(model.state_dict(), out / "ae.pt")
    (out / "config.json").write_text(
        json.dumps(
            {
                "features": FEATURES,
                "dim": 52,
                "hidden": [32, 16, 8, 16, 32],
                "train_runs": args.train_runs,
                "cal_runs": args.cal_runs,
                "alpha": args.alpha,
                "seed": args.seed,
                "epochs": args.epochs,
                "lr": args.lr,
                "threshold": threshold,
                "n_cal": int(cal_scores.shape[0]),
                "note": "simulated TE only; conformal guarantee approximate for time series",
            },
            indent=2,
        )
    )
    print(f"train_rows={x_train.shape[0]} n_cal={cal_scores.shape[0]} "
          f"threshold={threshold:.6f} alpha={args.alpha}")
    print(f"saved {out / 'ae.pt'} {out / 'scaler.npz'} {out / 'config.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
