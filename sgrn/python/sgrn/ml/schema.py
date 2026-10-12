#!/usr/bin/env python3
"""
sgrn.ml.schema — SCL Schema Parser, Semantic Taxonomy & Automated Model Configuration.

Translates Siemens S7 SCL/AWL schemas into:
  - Discovered continuous process variables
  - Physical dimension taxonomy (Pressure, Temperature, Flow, Level, Vibration, etc.)
  - Subsystem groupings (InletSeparation, AdsorberTowers, SafetySystems, etc.)
  - Auto-generated PyTorch Manifold model architecture topology
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

PHYSICAL_DIMENSION_MAP: Dict[str, str] = {
    # Pressure
    "bar": "Pressure",
    "mbar": "Pressure",
    "psi": "Pressure",
    "kpa": "Pressure",
    "mpa": "Pressure",
    # Temperature
    "°c": "Temperature",
    "c": "Temperature",
    "k": "Temperature",
    "°f": "Temperature",
    "degc": "Temperature",
    # Flow
    "dam³/h": "Flow",
    "m³/h": "Flow",
    "kg/h": "Flow",
    "l/min": "Flow",
    "gpm": "Flow",
    "sm3/d": "Flow",
    # Level / Saturation / Percentage
    "%": "Level",
    "pct": "Level",
    # Mechanical / Speed / Vibration
    "rpm": "Rotational_Speed",
    "hz": "Frequency",
    "mm/s": "Vibration",
    # Energy / Heat Value
    "mj/m³": "Energy",
    "btu/scf": "Energy",
    "kw": "Power",
    "mw": "Power",
    # Chemical Composition
    "ppm": "Composition",
    "ppmv": "Composition",
    "mol%": "Composition",
}


def inferPhysicalDimension(unit: Optional[str]) -> str:
    """Infer high-level physical dimension from engineering unit string."""
    if not unit:
        return "General"
    clean = unit.strip().lower()
    return PHYSICAL_DIMENSION_MAP.get(clean, "General")


def inferDimensionFromName(name: str) -> str:
    """Heuristic fallback to infer physical dimension from variable name."""
    n = name.lower()
    if any(k in n for k in ("press", "dp", "bar")):
        return "Pressure"
    if any(k in n for k in ("temp", "deg")):
        return "Temperature"
    if any(k in n for k in ("flow", "rate")):
        return "Flow"
    if any(k in n for k in ("level", "pct")):
        return "Level"
    if any(k in n for k in ("speed", "rpm")):
        return "Rotational_Speed"
    if any(k in n for k in ("vibr", "mm_s")):
        return "Vibration"
    return "General"


def parseSclSchema(schemaSource: Union[str, Path]) -> Dict[str, Any]:
    """Parse Siemens S7 SCL/AWL schema, resolving UDTs, DataBlocks, and pragmas.
    
    Extracts:
      - All DB data blocks and UDT types
      - Signal definitions with #UNIT, #RANGE, and comments
      - Expanded dot-path signal names (e.g. InletSeparation.feed_flow.rate)
      - Physical dimension classification for every signal
    """
    content = (
        Path(schemaSource).read_text(encoding="utf-8")
        if isinstance(schemaSource, (str, Path)) and Path(schemaSource).is_file()
        else str(schemaSource)
    )

    # 1. Parse UDTs
    type_blocks = re.findall(
        r'TYPE\s+\"([^\"]+)\".*?VAR(.*?)END_VAR\s*END_TYPE', content, re.DOTALL
    )
    types_dict: Dict[str, List[Dict[str, Any]]] = {}
    for tname, tbody in type_blocks:
        fields = []
        for line in tbody.strip().splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            m = re.match(r"([a-zA-Z0-9_]+)\s*:\s*([^;]+);(?:\s*//\s*(.*))?", line)
            if m:
                fname, ftype_str, comment = m.groups()
                u_match = re.search(r'#UNIT\(\"([^\"]+)\"\)', ftype_str)
                unit = u_match.group(1) if u_match else None
                clean_type = re.sub(r"#.*", "", ftype_str).strip().strip('"')
                fields.append({
                    "name": fname,
                    "type": clean_type,
                    "unit": unit,
                    "comment": (comment or "").strip(),
                })
        types_dict[tname] = fields

    # 2. Parse DATA_BLOCKs
    db_blocks = re.findall(
        r'DATA_BLOCK\s+\"([^\"]+)\"\s+DB\s+(\d+).*?VAR(.*?)END_VAR\s*END_DATA_BLOCK',
        content,
        re.DOTALL,
    )
    signals: Dict[str, Dict[str, Any]] = {}
    dbs: Dict[str, int] = {}

    for db_name, db_num_str, db_body in db_blocks:
        db_num = int(db_num_str)
        dbs[db_name] = db_num
        for line in db_body.strip().splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            m = re.match(r"([a-zA-Z0-9_]+)\s*:\s*([^;]+);(?:\s*//\s*(.*))?", line)
            if m:
                fname, ftype_str, comment = m.groups()
                u_match = re.search(r'#UNIT\(\"([^\"]+)\"\)', ftype_str)
                unit = u_match.group(1) if u_match else None
                clean_type = re.sub(r"#.*", "", ftype_str).strip().strip('"')

                if clean_type in types_dict:
                    for sub in types_dict[clean_type]:
                        sig_key = f"{db_name}.{fname}.{sub['name']}"
                        sig_unit = sub["unit"] or unit
                        signals[sig_key] = {
                            "db": db_name,
                            "db_number": db_num,
                            "name": f"{fname}.{sub['name']}",
                            "unit": sig_unit,
                            "dimension": inferPhysicalDimension(sig_unit),
                            "type": sub["type"],
                            "comment": sub["comment"] or (comment or "").strip(),
                        }
                elif clean_type.startswith("ARRAY"):
                    arr_m = re.match(
                        r"ARRAY\s*\[(\d+)\.\.(\d+)\]\s*OF\s*\"?([a-zA-Z0-9_]+)\"?", clean_type
                    )
                    if arr_m:
                        low, high, elem_type = (
                            int(arr_m.group(1)),
                            int(arr_m.group(2)),
                            arr_m.group(3),
                        )
                        if elem_type in types_dict:
                            for idx in range(low, high + 1):
                                for sub in types_dict[elem_type]:
                                    sig_key = f"{db_name}.{fname}[{idx}].{sub['name']}"
                                    sig_unit = sub["unit"] or unit
                                    signals[sig_key] = {
                                        "db": db_name,
                                        "db_number": db_num,
                                        "name": f"{fname}[{idx}].{sub['name']}",
                                        "unit": sig_unit,
                                        "dimension": inferPhysicalDimension(sig_unit),
                                        "type": sub["type"],
                                        "comment": sub["comment"] or (comment or "").strip(),
                                    }
                        else:
                            for idx in range(low, high + 1):
                                sig_key = f"{db_name}.{fname}[{idx}]"
                                signals[sig_key] = {
                                    "db": db_name,
                                    "db_number": db_num,
                                    "name": f"{fname}[{idx}]",
                                    "unit": unit,
                                    "dimension": inferPhysicalDimension(unit),
                                    "type": elem_type,
                                    "comment": (comment or "").strip(),
                                }
                else:
                    sig_key = f"{db_name}.{fname}"
                    signals[sig_key] = {
                        "db": db_name,
                        "db_number": db_num,
                        "name": fname,
                        "unit": unit,
                        "dimension": inferPhysicalDimension(unit),
                        "type": clean_type,
                        "comment": (comment or "").strip(),
                    }

    return {
        "dataBlocks": dbs,
        "types": types_dict,
        "signals": signals,
    }


def configureModelFromSchema(
    schemaSource: Union[str, Path],
    includeSubsystems: Optional[List[str]] = None,
    excludeSubsystems: Optional[List[str]] = None,
    includeDimensions: Optional[List[str]] = None,
    windowSize: int = 12,
    hiddenDim: Optional[int] = None,
    latentDim: Optional[int] = None,
    alpha: float = 0.05,
    kMultiplier: float = 1.4,
) -> Dict[str, Any]:
    """Automatically configure PyTorch Manifold model architecture, feature space,
    and multi-dimensional attribution groups directly from an SCL schema.
    
    Filters out non-continuous and control feedback registers (e.g. timestamps,
    discrete alarms, esd_signal, and model trip feedback registers like model_anomaly,
    model_trip_advice, model_fault_code).
    
    Returns:
      - 'features': List[str] of chosen process telemetry signals
      - 'featureDimensions': Dict[str, str] mapping signal -> physical dimension
      - 'featureSubsystems': Dict[str, str] mapping signal -> plant subsystem
      - 'dimensionGroups': Dict[str, List[str]] mapping physical dimension -> signals
      - 'subsystemGroups': Dict[str, List[str]] mapping plant subsystem -> signals
      - 'modelConfig': Dict with auto-calculated network layer topology
    """
    parsed = parseSclSchema(schemaSource)
    signals = parsed["signals"]

    # Exclude model feedback registers, trips, discrete alarms, and timestamps
    control_blacklist = {
        "timestamp", "esd_signal", "model_trip_advice", "model_anomaly",
        "model_fault_code", "esd_manual", "esd_auto", "esd_reason_code",
        "esd_time_ms", "active_alarm_count", "any_critical", "dew_point_forecast",
        "dew_point_forecast_std", "dew_point_spec", "trip_high_press", "trip_low_press",
        "trip_high_temp", "trip_high_dp",
    }

    selected_features: List[str] = []
    feature_dimensions: Dict[str, str] = {}
    feature_subsystems: Dict[str, str] = {}
    dimension_groups: Dict[str, List[str]] = {}
    subsystem_groups: Dict[str, List[str]] = {}

    for sig_name, sig_meta in signals.items():
        base_name = sig_meta["name"].split("[")[0].split(".")[-1]
        if base_name in control_blacklist or sig_meta["type"] not in ("Real", "LReal"):
            continue

        db = sig_meta["db"]
        dim = sig_meta["dimension"]

        if includeSubsystems and db not in includeSubsystems:
            continue
        if excludeSubsystems and db in excludeSubsystems:
            continue
        if includeDimensions and dim not in includeDimensions:
            continue

        selected_features.append(sig_name)
        feature_dimensions[sig_name] = dim
        feature_subsystems[sig_name] = db
        dimension_groups.setdefault(dim, []).append(sig_name)
        subsystem_groups.setdefault(db, []).append(sig_name)

    num_features = len(selected_features)
    input_dim = windowSize * num_features

    # Calculate optimal network geometry
    if hiddenDim is None:
        hiddenDim = max(32, min(128, int(input_dim // 3)))
    if latentDim is None:
        latentDim = max(8, min(32, int(hiddenDim // 4)))

    model_config = {
        "window_size": windowSize,
        "input_dim": input_dim,
        "num_features": num_features,
        "hidden_dim": hiddenDim,
        "latent_dim": latentDim,
        "alpha": alpha,
        "k_multiplier": kMultiplier,
    }

    return {
        "features": selected_features,
        "featureDimensions": feature_dimensions,
        "featureSubsystems": feature_subsystems,
        "dimensionGroups": dimension_groups,
        "subsystemGroups": subsystem_groups,
        "modelConfig": model_config,
        # snake_case backward-compatibility keys
        "feature_dimensions": feature_dimensions,
        "feature_subsystems": feature_subsystems,
        "dimension_groups": dimension_groups,
        "subsystem_groups": subsystem_groups,
        "model_config": model_config,
    }


# Backward-compatible snake_case aliases
infer_physical_dimension = inferPhysicalDimension
infer_dimension_from_name = inferDimensionFromName
parse_scl_schema = parseSclSchema
configure_model_from_schema = configureModelFromSchema
