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


if __name__ == "__main__":
    import sys
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("config.yaml")
    cfg = loadConfig(path)
    print(json.dumps(cfg, indent=2))