#!/usr/bin/env python3
"""Config loader supporting JSON and YAML formats."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


def loadConfig(config_path: Path) -> Dict[str, Any]:
    """Load configuration from JSON or YAML file."""
    if not config_path.is_file():
        raise FileNotFoundError(f"Config file not found: {config_path}")

    text = config_path.read_text(encoding="utf-8")

    if config_path.suffix in (".yaml", ".yml"):
        if not HAS_YAML:
            raise ImportError("PyYAML required for YAML config. Install: pip install pyyaml")
        return yaml.safe_load(text)
    elif config_path.suffix == ".json":
        return json.loads(text)
    else:
        # Try YAML first, then JSON
        if HAS_YAML:
            try:
                return yaml.safe_load(text)
            except yaml.YAMLError:
                pass
        return json.loads(text)


def getConfigPath(base_dir: Path) -> Path:
    """Find config file, preferring YAML over JSON."""
    yaml_path = base_dir / "config.yaml"
    json_path = base_dir / "config.json"

    if yaml_path.is_file():
        return yaml_path
    if json_path.is_file():
        return json_path
    # Default to JSON path for creation
    return json_path


def _getWindowSize(config: dict) -> int:
    """Extract window_size from config, supporting both legacy int and new dict format."""
    ws = config.get("dataset", {}).get("window_size", 12)
    if isinstance(ws, dict):
        return int(ws.get("size", 12))
    return int(ws)


def _getTicksPerDay(config: dict) -> int:
    """Simulation ticks per simulated day (24 → 1 tick = 1 simulated hour;
    1440 → 1 tick = 1 simulated minute)."""
    return int(config.get("scenario", {}).get("ticks_per_day", 24))


def _getWindowMinutes(config: dict) -> float:
    """Length of one sliding window in simulated minutes."""
    ds = config.get("dataset", {})
    ws = ds.get("window_size", 12)
    if isinstance(ws, dict):
        size = float(ws.get("size", 12))
        unit = str(ws.get("unit", "hours")).lower()
    else:
        size, unit = float(ws), "hours"
    if unit.startswith("min"):
        return size
    if unit.startswith("hour"):
        return size * 60.0
    if unit.startswith("day"):
        return size * 1440.0
    raise ValueError(
        f"Unsupported window_size unit {unit!r}; use 'minutes', 'hours' or 'days'")


def getWindowRows(config: dict) -> int:
    """Sliding-window length in DATA ROWS (ticks), unit-aware.

    ``window_size: {unit: minutes, size: 1}`` over minute-tick simulation
    (ticks_per_day=1440) → 1 row. ``{unit: hours, size: 12}`` over hourly
    ticks (24/day) → 12 rows, matching the legacy integer behaviour exactly:
    12 * 60 min / (1440/24 min per tick) = 12.
    """
    window_min = _getWindowMinutes(config)
    tick_period_min = (24.0 * 60.0) / _getTicksPerDay(config)
    return max(1, int(round(window_min / tick_period_min)))


if __name__ == "__main__":
    import sys
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("config.yaml")
    cfg = loadConfig(path)
    print(json.dumps(cfg, indent=2))