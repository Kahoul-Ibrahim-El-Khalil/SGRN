#!/usr/bin/env python3
"""Unified launcher for the FLARE_BLOCKED experiment.

Run from the repository root:

  python3 experiments/flare check
  python3 experiments/flare run
  python3 experiments/flare demo
  python3 experiments/flare evaluate

Or: python3 experiments/flare --help
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence

FLARE_DIR = Path(__file__).resolve().parent
REPO_ROOT = FLARE_DIR.parents[1]

ARCHIVE = FLARE_DIR / "flare_history.jsonl.zst"
META = FLARE_DIR / "meta.json"
MODEL = FLARE_DIR / "model" / "model.pt"
MODEL_META = FLARE_DIR / "model" / "model_meta.json"

BINARY_NAMES = ("gateway", "s7shell", "sgrn_replay", "sgrn_dataset")


def _python() -> str:
    return sys.executable


def _script(name: str) -> Path:
    path = FLARE_DIR / name
    if not path.is_file():
        raise FileNotFoundError(f"Missing script: {path}")
    return path


def _runScript(script: str, extra: Sequence[str], *, cwd: Optional[Path] = None) -> int:
    cmd = [_python(), str(_script(script)), *extra]
    print(f"[flare] {' '.join(cmd)}")
    return subprocess.call(cmd, cwd=str(cwd or REPO_ROOT))


def _findBinary(name: str) -> Optional[Path]:
    app_name = {"sgrn_replay": "replay"}.get(name, name)
    candidates = (
        REPO_ROOT / ".dist/linux-static-release" / name,
        REPO_ROOT / ".dist/linux-static" / name,
        REPO_ROOT / ".prefix/bin" / name,
        REPO_ROOT / ".prefix/bin" / app_name,
    )
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None


def _activeConfigPath() -> Path:
    from config_loader import getConfigPath

    return getConfigPath(FLARE_DIR)


def _moduleAvailable(name: str) -> bool:
    return importlib.util.find_spec(name) is not None


def cmdCheck(_args: argparse.Namespace) -> int:
    from config_loader import getConfigPath, loadConfig

    ok = True
    print("SGRN flare experiment — environment check")
    print(f"  Repo root:     {REPO_ROOT}")
    print(f"  Experiment:    {FLARE_DIR}")
    print()

    if not (REPO_ROOT / "sgrn/python").is_dir():
        print("  ✗ sgrn/python not found (run from SGRN repository root)")
        ok = False
    else:
        print("  ✓ Repository layout")

    cfg_path = getConfigPath(FLARE_DIR)
    if cfg_path.is_file():
        try:
            cfg = loadConfig(cfg_path)
            ws = cfg.get("dataset", {}).get("window_size", 12)
            if isinstance(ws, dict):
                ws = ws.get("size", ws)
            print(f"  ✓ Config: {cfg_path.name} (window_size={ws})")
        except Exception as exc:
            print(f"  ✗ Config {cfg_path.name}: {exc}")
            ok = False
    else:
        print("  ✗ No config.yaml or config.json in experiments/flare/")
        ok = False

    yaml_cfg = FLARE_DIR / "config.yaml"
    json_cfg = FLARE_DIR / "config.json"
    if yaml_cfg.is_file() and json_cfg.is_file():
        print("  ℹ Both config.yaml and config.json exist; YAML is used at runtime.")

    for mod in ("torch", "numpy", "pandas", "tqdm", "matplotlib"):
        if _moduleAvailable(mod):
            print(f"  ✓ Python: {mod}")
        else:
            print(f"  ✗ Python: {mod} (install in your env)")
            ok = False
    if not _moduleAvailable("yaml"):
        print("  ⚠ Python: PyYAML optional (needed only for config.yaml)")

    if ARCHIVE.is_file():
        mb = ARCHIVE.stat().st_size / (1024 * 1024)
        print(f"  ✓ WAL archive: {ARCHIVE.name} ({mb:.2f} MB)")
    else:
        print(f"  ⚠ WAL archive missing — run: python3 experiments/flare capture  (or run --capture)")

    if META.is_file():
        print(f"  ✓ Scenario meta: {META.name}")
    elif not ARCHIVE.is_file():
        ok = False

    if MODEL.is_file() and MODEL_META.is_file():
        print("  ✓ Trained model: model.pt + model_meta.json")
    else:
        print("  ⚠ Model not trained — run: python3 experiments/flare run")

    print()
    print("  Native binaries (capture / demo):")
    for name in BINARY_NAMES:
        path = _findBinary(name)
        if path:
            print(f"  ✓ {name}: {path.relative_to(REPO_ROOT)}")
        else:
            print(f"  ⚠ {name}: not found (build SGRN or use prebuilt .dist/)")

    print()
    if ok:
        print("Ready. Typical flows:")
        print("  python3 experiments/flare run          # full ML pipeline (uses bundled WAL)")
        print("  python3 experiments/flare evaluate     # metrics + plot only")
        print("  python3 experiments/flare demo         # live replay (needs gateway + replay binaries)")
    else:
        print("Fix the items marked ✗ before running the pipeline.")
    return 0 if ok else 1


def cmdRun(args: argparse.Namespace) -> int:
    extra: List[str] = []
    if args.output_dir:
        extra.extend(["--output-dir", args.output_dir])
    if args.capture:
        extra.append("--capture")
    if args.epochs is not None:
        extra.extend(["--epochs", str(args.epochs)])
    if args.no_plot:
        extra.append("--no-plot")
    extra.extend(args.extra)
    return _runScript("orchestrate.py", extra)


def cmdTrain(args: argparse.Namespace) -> int:
    extra: List[str] = []
    if args.save_dir:
        extra.extend(["--save-dir", args.save_dir])
    extra.extend(args.extra)
    return _runScript("train_detector.py", extra)


def cmdEvaluate(args: argparse.Namespace) -> int:
    extra: List[str] = []
    if args.no_plot:
        extra.append("--no-plot")
    if args.model_dir:
        extra.extend(["--model-dir", args.model_dir])
    if args.output_dir:
        extra.extend(["--output-dir", args.output_dir])
    extra.extend(args.extra)
    return _runScript("evaluate.py", extra)


def cmdDemo(args: argparse.Namespace) -> int:
    extra: List[str] = ["--speed", str(args.speed)]
    if args.model_dir:
        extra.extend(["--model-dir", args.model_dir])
    if not args.llm:
        extra.append("--no-llm")
    if args.verbose:
        extra.append("--verbose")
    extra.extend(args.extra)
    return _runScript("run_demo.py", extra)


def cmdCapture(args: argparse.Namespace) -> int:
    extra: List[str] = []
    if args.out:
        extra.extend(["--out", args.out])
    extra.extend(args.extra)
    return _runScript("run_capture.py", extra)


def buildParser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="experiments/flare",
        description="FLARE_BLOCKED anomaly detection experiment — one entry point for check, train, evaluate, demo, and full pipeline.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
What is FLARE_BLOCKED?
-------------------
FLARE_BLOCKED is an anomaly detection experiment for gas processing plants.
It uses a physics-informed split-conformal manifold autoencoder to detect
flare-blockage events in a 31-day (742-hour) natural gas molecular sieve plant
simulation.

Pipeline flow:
  1. check       — Verify repo layout, Python deps, config, artifacts, and binaries
  2. capture     — Regenerate WAL from simulation (needs gateway, s7shell, sgrn_replay)
  3. run         — End-to-end pipeline: dataset → train → evaluate → report (orchestrate.py)
  4. train       — Train autoencoder + conformal calibration only
  5. evaluate    — Offline metrics and plot (requires model.pt)
  6. demo        — Live replay + detector + advisory agent (needs gateway, s7shell, sgrn_replay)

How it works:
  • Stage 1: s7shell soft-PLC simulation produces flare_history.jsonl.zst WAL archive
  • Stage 2: sgrn_dataset binary converts WAL → CSV with sliding windows (12-hour windows)
  • Stage 3: WindowAutoencoder trained on 70% pre-fault data, split-conformal calibration on 30%
  • Stage 4: Sliding-window scan across full 742h timeline, detection delay & ESD lead time metrics
  • Stage 5: Executive diagnostic report generation

Typical usage (from repository root):
  python3 experiments/flare check              # verify environment
  python3 experiments/flare capture            # regenerate WAL from simulation
  python3 experiments/flare run                # full ML pipeline (uses bundled WAL)
  python3 experiments/flare run --output-dir scratch/flare_try --epochs 20
  python3 experiments/flare evaluate           # metrics + plot only
  python3 experiments/flare demo               # live replay (needs gateway + replay binaries)
  python3 experiments/flare demo --speed 10 --llm

The bundled WAL (flare_history.jsonl.zst) is enough for `run` and `evaluate`
without rebuilding native binaries. `capture` and `demo` need gateway, s7shell,
and sgrn_replay in .dist/ or .prefix/bin/.

Output directory:
  --output-dir DIR   Artifact directory (default: experiments/flare/)
                   Datasets go to <DIR>/dataset/ and models to <DIR>/model/
                   Use this to avoid polluting the source tree.

Examples:
  python3 experiments/flare check
  python3 experiments/flare run
  python3 experiments/flare run --output-dir scratch/flare_try --epochs 20
  python3 experiments/flare evaluate
  python3 experiments/flare demo
  python3 experiments/flare demo --speed 10 --llm
  python3 experiments/flare capture
""",
    )
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    sub.add_parser("check", help="Verify repo, Python deps, config, artifacts, and binaries")

    p_run = sub.add_parser("run", help="End-to-end pipeline: dataset → train → evaluate → report (orchestrate.py)")
    p_run.add_argument("--output-dir", default=None, help=f"Artifact directory (default: {FLARE_DIR.name}/)")
    p_run.add_argument("--capture", action="store_true", help="Re-run soft-PLC simulation before training (slow)")
    p_run.add_argument("--epochs", type=int, default=None, help="Training epochs (default: 40)")
    p_run.add_argument("--no-plot", action="store_true", help="Skip evaluation plot")
    p_run.add_argument("extra", nargs=argparse.REMAINDER, help="Extra args passed to orchestrate.py")

    p_train = sub.add_parser("train", help="Train autoencoder + conformal calibration only")
    p_train.add_argument("--save-dir", default=None,
                         help="Parent dir receiving a 'model/' subfolder with model.pt + model_meta.json")
    p_train.add_argument("extra", nargs=argparse.REMAINDER, help="Extra args for train_detector.py")

    p_eval = sub.add_parser("evaluate", help="Offline metrics and plot (requires model.pt)")
    p_eval.add_argument("--no-plot", action="store_true")
    p_eval.add_argument("--model-dir", default=None,
                        help="Directory holding model.pt + model_meta.json (default: flare model/)")
    p_eval.add_argument("--output-dir", default=None,
                        help="Directory for evaluation_metrics.json and evaluation_plot.png")
    p_eval.add_argument("extra", nargs=argparse.REMAINDER)

    p_demo = sub.add_parser("demo", help="Live replay + detector + advisory agent")
    p_demo.add_argument("--speed", type=float, default=60.0, help="Replay speed multiplier (default: 60)")
    p_demo.add_argument("--model-dir", default=None,
                        help="Directory holding model.pt + model_meta.json (default: flare model/)")
    p_demo.add_argument("--llm", action="store_true", help="Call OpenAI (requires OPENAI_API_KEY); default is template-only")
    p_demo.add_argument("--verbose", action="store_true", help="Log gateway/replay subprocess output")
    p_demo.add_argument("extra", nargs=argparse.REMAINDER)

    p_cap = sub.add_parser("capture", help="Regenerate WAL from simulation (needs native binaries)")
    p_cap.add_argument("--out", default=None, help="Output directory")
    p_cap.add_argument("--verbose", action="store_true")
    p_cap.add_argument("extra", nargs=argparse.REMAINDER)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    if str(REPO_ROOT) not in sys.path:
        sys.path.insert(0, str(FLARE_DIR))
    parser = buildParser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    if not args.command:
        parser.print_help()
        return 0

    handlers = {
        "check": cmdCheck,
        "run": cmdRun,
        "train": cmdTrain,
        "evaluate": cmdEvaluate,
        "demo": cmdDemo,
        "capture": cmdCapture,
    }
    return handlers[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
