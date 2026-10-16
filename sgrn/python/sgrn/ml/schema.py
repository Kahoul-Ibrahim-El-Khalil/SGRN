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
    # Dimensionless ratio / saturation / valve & speed setpoints.
    # NOTE: "%" alone carries no physical dimension — callers needing
    # Level vs. valve-position vs. speed-setpoint disambiguation should use
    # inferDimensionFromName() on the signal name. Classifying bare "%" as
    # "Level" mislabels e.g. valve position and speed setpoints.
    "%": "Percent",
    "pct": "Percent",
    "percent": "Percent",
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


def _extractEnum(ftype_str: str) -> List[str]:
    """Extract ``#ENUM(...)`` category names in source order (``NAME=val`` -> ``NAME``)."""
    m = re.search(r"#enum\(\s*([^)]+)\)", ftype_str, re.IGNORECASE)
    if not m:
        return []
    cats = []
    for token in m.group(1).split(","):
        name = token.split("=")[0].strip().strip('"')
        if name:
            cats.append(name)
    return cats


def _extractPragmas(ftype_str: str) -> Dict[str, Any]:
    """Extract #UNIT / #RANGE / #MIN / #MAX pragmas case-insensitively."""
    unit_m = re.search(r'#unit\(\"([^\"]+)\"\)', ftype_str, re.IGNORECASE)
    unit = unit_m.group(1) if unit_m else None
    min_v = max_v = None
    range_m = re.search(r'#range\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*\)', ftype_str, re.IGNORECASE)
    if range_m:
        try:
            min_v, max_v = float(range_m.group(1)), float(range_m.group(2))
        except ValueError:
            pass
    else:
        min_m = re.search(r'#min\(\s*([-\d.eE+]+)\s*\)', ftype_str, re.IGNORECASE)
        max_m = re.search(r'#max\(\s*([-\d.eE+]+)\s*\)', ftype_str, re.IGNORECASE)
        if min_m:
            try:
                min_v = float(min_m.group(1))
            except ValueError:
                pass
        if max_m:
            try:
                max_v = float(max_m.group(1))
            except ValueError:
                pass
    return {"unit": unit, "min_value": min_v, "max_value": max_v}


def parseSclSchema(schemaSource: Union[str, Path, Dict[str, Any]]) -> Dict[str, Any]:
    """Parse Siemens S7 SCL/AWL schema, resolving UDTs, DataBlocks, and pragmas.

    Offline fallback path — prefer :func:`signalsFromRegistry` whenever
    gateway registry JSON or a ``.bin.zst`` embedded schema is available.
    A registry-shaped dict passed here is converted directly.
    
    Extracts:
      - All DB data blocks and UDT types
      - Signal definitions with #UNIT, #RANGE, and comments
      - Expanded dot-path signal names (e.g. InletSeparation.feed_flow.rate)
      - Physical dimension classification for every signal
    """
    if isinstance(schemaSource, dict):
        return signalsFromRegistry(schemaSource)
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
                pragmas = _extractPragmas(ftype_str)
                unit = pragmas["unit"]
                clean_type = re.sub(r"#.*", "", ftype_str).strip().strip('"')
                fields.append({
                    "name": fname,
                    "type": clean_type,
                    "unit": unit,
                    "min_value": pragmas["min_value"],
                    "max_value": pragmas["max_value"],
                    "enum": _extractEnum(ftype_str),
                    "comment": (comment or "").strip(),
                })
        types_dict[tname] = fields

    # 1b. Parse named scalar-type aliases with enums, e.g.
    #     TYPE "PlantMode" : USInt #ENUM(STOPPED=0, STARTING=1, ...)
    # These have no VAR block, so the UDT regex above misses them — yet
    # fields referencing the alias (e.g. ``mode : "PlantMode"``) carry a
    # discrete operating mode that is prime conditioning context.
    type_aliases: Dict[str, Dict[str, Any]] = {}
    for line in content.splitlines():
        am = re.match(
            r'\s*TYPE\s+"([^"]+)"\s+(?:#[A-Za-z0-9_]+\s+)*:\s*([A-Za-z0-9_]+)\s*(#ENUM\(.*\))?\s*$',
            line,
        )
        if am:
            aname, base, enum_part = am.groups()
            if aname not in types_dict:
                type_aliases[aname] = {
                    "base": base,
                    "enum": _extractEnum(enum_part or ""),
                }

    def _resolveAlias(clean_type: str) -> tuple[str, List[str]]:
        """Resolve a named scalar alias to (base_type, enum_categories)."""
        alias = type_aliases.get(clean_type.strip().strip('"'))
        if alias is not None:
            return alias["base"], list(alias["enum"])
        return clean_type, []

    # 2. Parse DATA_BLOCKs (DB number may be glued: "DB1" or spaced: "DB 1")
    db_blocks = re.findall(
        r'DATA_BLOCK\s+\"([^\"]+)\"\s+DB\s*(\d+).*?VAR(.*?)END_VAR\s*END_DATA_BLOCK',
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
                pragmas = _extractPragmas(ftype_str)
                unit = pragmas["unit"]
                min_v, max_v = pragmas["min_value"], pragmas["max_value"]
                field_enum = _extractEnum(ftype_str)
                clean_type = re.sub(r"#.*", "", ftype_str).strip().strip('"')
                clean_type, alias_enum = _resolveAlias(clean_type)
                if alias_enum:
                    field_enum = alias_enum

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
                            "min_value": sub.get("min_value", min_v),
                            "max_value": sub.get("max_value", max_v),
                            "enum": list(sub.get("enum", [])),
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
                        elem_base, elem_alias_enum = _resolveAlias(elem_type)
                        if elem_base in types_dict:
                            for idx in range(low, high + 1):
                                for sub in types_dict[elem_base]:
                                    sig_key = f"{db_name}.{fname}[{idx}].{sub['name']}"
                                    sig_unit = sub["unit"] or unit
                                    signals[sig_key] = {
                                        "db": db_name,
                                        "db_number": db_num,
                                        "name": f"{fname}[{idx}].{sub['name']}",
                                        "unit": sig_unit,
                                        "dimension": inferPhysicalDimension(sig_unit),
                                        "type": sub["type"],
                                        "enum": list(sub.get("enum", [])),
                                        "comment": sub["comment"] or (comment or "").strip(),
                                    }
                        elif elem_alias_enum:
                            for idx in range(low, high + 1):
                                sig_key = f"{db_name}.{fname}[{idx}]"
                                signals[sig_key] = {
                                    "db": db_name,
                                    "db_number": db_num,
                                    "name": f"{fname}[{idx}]",
                                    "unit": unit,
                                    "dimension": inferPhysicalDimension(unit),
                                    "type": elem_base,
                                    "enum": list(elem_alias_enum),
                                    "comment": (comment or "").strip(),
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
                                    "enum": [],
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
                        "min_value": min_v,
                        "max_value": max_v,
                        "enum": list(field_enum),
                        "comment": (comment or "").strip(),
                    }

    return {
        "dataBlocks": dbs,
        "types": types_dict,
        "typeAliases": type_aliases,
        "signals": signals,
    }


def signalsFromRegistry(registry: Dict[str, Any]) -> Dict[str, Any]:
    """Convert a gateway registry / embedded-schema dict to parsed signals.

    This is the primary schema path: ``GET /registry`` and the ``.bin.zst``
    embedded header already carry offsets, ``unit``, ``dimension``, ``min`` /
    ``max``, ``enum`` (int -> name), ``endianness`` and ``read_only`` /
    ``transient`` flags — everything the SCL-text parser re-derives lossily
    (and has drifted on before). Accepts ``{"dbs": [...]}`` (registry style,
    ``db_number``/``db_name``), ``{"dbs": {...}}`` (embedded style) and
    ``{"data_blocks": [...]}``. UDTs resolve via ``"udts"``/``"types"``.
    Returns the same ``{"dataBlocks", "types", "signals"}`` shape as
    :func:`parseSclSchema` (signals additionally carry ``enum`` category
    lists, ``read_only`` and ``kind`` = continuous/discrete/timestamp).
    """
    raw_dbs = registry.get("dbs", registry.get("data_blocks", {}))
    if isinstance(raw_dbs, list):
        db_list = raw_dbs
    elif isinstance(raw_dbs, dict):
        db_list = list(raw_dbs.values())
    else:
        db_list = []

    raw_udts = registry.get("udts", registry.get("types", {}))
    if isinstance(raw_udts, list):
        udt_map = {u.get("name", ""): u for u in raw_udts if isinstance(u, dict)}
    elif isinstance(raw_udts, dict):
        udt_map = raw_udts
    else:
        udt_map = {}

    signals: Dict[str, Dict[str, Any]] = {}
    dbs: Dict[str, int] = {}

    def _enum_cats(enum_node: Any) -> List[str]:
        if isinstance(enum_node, dict):
            try:
                order = sorted(enum_node.keys(), key=lambda k: int(k))
            except (TypeError, ValueError):
                order = sorted(enum_node.keys())
            return [str(enum_node[k]) for k in order]
        if isinstance(enum_node, list):
            return [str(c) for c in enum_node]
        return []

    def _emit(db_name: str, db_num: int, path: str, node: Dict[str, Any],
              inherited_unit: Optional[str] = None) -> None:
        ftype = str(node.get("type", "Real"))
        unit = node.get("unit", inherited_unit)
        enum_cats = _enum_cats(node.get("enum"))
        dim = node.get("dimension") or inferPhysicalDimension(unit)
        min_v = node.get("min", node.get("min_val", node.get("min_value")))
        max_v = node.get("max", node.get("max_val", node.get("max_value")))
        try:
            min_v = float(min_v) if min_v is not None else None
        except (TypeError, ValueError):
            min_v = None
        try:
            max_v = float(max_v) if max_v is not None else None
        except (TypeError, ValueError):
            max_v = None
        tu = ftype.upper().strip().strip('"')
        kind = "continuous" if tu in ("REAL", "LREAL") else (
            "timestamp" if tu in ("DTL", "LDTL", "LDT", "DT", "DATE", "TOD") else "discrete"
        )
        sig_key = f"{db_name}.{path}"
        signals[sig_key] = {
            "db": db_name,
            "db_number": db_num,
            "name": path,
            "unit": unit,
            "dimension": dim,
            "type": ftype,
            "min_value": min_v,
            "max_value": max_v,
            "enum": enum_cats,
            "read_only": bool(node.get("read_only", node.get("is_read_only", False))),
            "kind": kind,
            "comment": str(node.get("description", node.get("comment", "") or "")),
        }

    def _walk(db_name: str, db_num: int, fields: List[Dict[str, Any]], prefix: str,
              inherited_unit: Optional[str] = None) -> None:
        for f in fields:
            if not isinstance(f, dict):
                continue
            if f.get("is_transient"):
                continue  # scratch state, not process telemetry
            fname = str(f.get("name", ""))
            if not fname:
                continue
            path = f"{prefix}.{fname}" if prefix else fname
            children = f.get("children") or []
            udt_name = f.get("udt_name")
            udt = udt_map.get(udt_name) if udt_name else None
            sub_fields = None
            if children:
                sub_fields = children
            elif isinstance(udt, dict):
                sub_fields = udt.get("fields", [])
            if sub_fields:
                _walk(db_name, db_num, sub_fields, path,
                      inherited_unit=f.get("unit", inherited_unit))
                continue
            count = 0
            try:
                count = int(f.get("count", 1) or 1)
            except (TypeError, ValueError):
                count = 1
            if count > 1 and not children and udt is None:
                for i in range(count):
                    _emit(db_name, db_num, f"{path}[{i}]", f,
                          inherited_unit=inherited_unit)
            else:
                _emit(db_name, db_num, path, f, inherited_unit=inherited_unit)

    for db in db_list:
        if not isinstance(db, dict):
            continue
        db_num = int(db.get("db_number", db.get("number", 0)) or 0)
        db_name = str(db.get("db_name", db.get("name", f"DB{db_num}")))
        dbs[db_name] = db_num
        _walk(db_name, db_num, db.get("fields", []), "")

    return {"dataBlocks": dbs, "types": {}, "signals": signals}


# Generic exclusion patterns for non-process signals. These replace the old
# hard-coded gas-plant literal list (dew_point_forecast, trip_high_press,
# …): model-feedback registers, trip setpoints, forecasts/specs, ESD logic,
# alarm aggregates and timestamps are excluded by *role*, not by plant, so
# the same defaults work for motor, nuclear, bottling, gas-processing, etc.
# Plant-specific extras belong in ``excludeSignals`` (exact signal keys or
# base field names), not in this shared default.
_EXCLUDE_EXACT = {
    "timestamp",
}

_EXCLUDE_PREFIXES = (
    "model_",  # model feedback registers (anomaly, trip advice, fault code…)
    "trip_",  # trip setpoints (over-pressure, runaway…)
    "esd_",  # emergency-shutdown logic
    "alarm",  # alarm aggregates/tables
)

_EXCLUDE_SUBSTRINGS = (
    "forecast",  # model forecasts + uncertainty (dew-point, quality…)
    "_spec",  # contractual setpoints/specs, not measurements
    "alarm",  # discrete alarm / trip discretes
    "critical",  # aggregate critical flags
    "fault_code",  # coded fault words, not continuous telemetry
)


def _isExcludedSignal(base_name: str, extra_exclude: Optional[List[str]] = None) -> bool:
    """True when a leaf field name is a non-process / feedback register."""
    n = base_name.strip().lower()
    if n in _EXCLUDE_EXACT:
        return True
    if n.startswith(_EXCLUDE_PREFIXES):
        return True
    if any(s in n for s in _EXCLUDE_SUBSTRINGS):
        return True
    if extra_exclude:
        lowered = {e.strip().lower() for e in extra_exclude}
        if n in lowered or base_name in (extra_exclude or []):
            return True
    return False


# S7 integer types eligible as enum context once an #ENUM (inline or via a
# named scalar alias) fixes their category set. High-cardinality counters
# without an enum stay out — their code distance is as meaningless as
# stuffing them into the MSE.
_CONTEXT_INT_TYPES = frozenset({
    "SINT", "USINT", "INT", "UINT", "DINT", "UDINT",
    "BYTE", "WORD", "DWORD", "LINT", "ULINT", "LWORD",
})


def configureModelFromSchema(
    schemaSource: Union[str, Path, Dict[str, Any]],
    includeSubsystems: Optional[List[str]] = None,
    excludeSubsystems: Optional[List[str]] = None,
    includeDimensions: Optional[List[str]] = None,
    windowSize: int = 12,
    hiddenDim: Optional[int] = None,
    latentDim: Optional[int] = None,
    alpha: float = 0.05,
    kMultiplier: float = 1.4,
    excludeSignals: Optional[List[str]] = None,
    includeContext: bool = True,
    maxContextCardinality: int = 32,
) -> Dict[str, Any]:
    """Automatically configure PyTorch Manifold model architecture, feature space,
    and multi-dimensional attribution groups directly from an SCL schema.

    Source priority (first match wins):
      1. A parsed/registry dict (``GET /registry`` JSON, ``.bin.zst``
         embedded schema, or ``{"signals": ...}``) — authoritative: real
         offsets, resolved enums/aliases, ``dimension`` and ``min``/``max``.
      2. A ``.json`` file path — loaded as (1).
      3. SCL text / ``.scl`` path — offline fallback re-parse (lossy: no
         offsets, enums only from ``#ENUM`` pragmas).

    Only continuous ``Real``/``LReal`` telemetry is selected. Non-process
    registers (timestamps, model-feedback outputs, trip setpoints,
    forecasts/specs, ESD logic, alarm aggregates — see ``_EXCLUDE_*``) are
    excluded by role, not by plant: the same defaults apply to motor,
    nuclear, bottling and gas-processing schemas. Pass plant-specific leaf
    names or full ``"Db.field"`` keys in ``excludeSignals`` to extend the
    default without forking this module.

    Discrete operating context (``Bool`` flags and ``#ENUM`` integers such as
    ``plant_mode``) is discovered alongside and returned as
    ``contextFeatures``/``contextSpecs`` — conditioning inputs for the
    manifold autoencoder, never reconstruction targets. Set
    ``includeContext=False`` for the legacy continuous-only return shape.
    DTL/LDTL timestamp fields are reported as ``timestampFields`` (never
    features) so pipelines can normalise time directly instead of via CSV.

    Returns:
      - 'features': List[str] of chosen process telemetry signals
      - 'featureDimensions': Dict[str, str] mapping signal -> physical dimension
      - 'featureSubsystems': Dict[str, str] mapping signal -> plant subsystem
      - 'dimensionGroups': Dict[str, List[str]] mapping physical dimension -> signals
      - 'subsystemGroups': Dict[str, List[str]] mapping plant subsystem -> signals
      - 'contextFeatures': List[str] of discrete conditioning signals
      - 'contextSpecs': Dict[str, {kind, categories}] per context signal
      - 'timestampFields': List[str] of DTL/LDTL timestamp signals
      - 'modelConfig': Dict with auto-calculated network layer topology
    """
    if isinstance(schemaSource, dict):
        if "signals" in schemaSource and isinstance(schemaSource["signals"], dict):
            signals = schemaSource["signals"]
        else:
            signals = signalsFromRegistry(schemaSource)["signals"]
    elif isinstance(schemaSource, (str, Path)) and str(schemaSource).endswith(".json") \
            and Path(schemaSource).is_file():
        import json as _json

        signals = signalsFromRegistry(
            _json.loads(Path(schemaSource).read_text(encoding="utf-8"))
        )["signals"]
    else:
        parsed = parseSclSchema(schemaSource)
        signals = parsed["signals"]

    extra_lower = {e.strip().lower() for e in (excludeSignals or [])}

    def _excluded(sig_name: str, base_name: str) -> bool:
        return (
            base_name in (excludeSignals or [])
            or sig_name in (excludeSignals or [])
            or base_name.lower() in extra_lower
            or sig_name.lower() in extra_lower
            or _isExcludedSignal(base_name)
        )

    selected_features: List[str] = []
    feature_dimensions: Dict[str, str] = {}
    feature_subsystems: Dict[str, str] = {}
    dimension_groups: Dict[str, List[str]] = {}
    subsystem_groups: Dict[str, List[str]] = {}
    context_features: List[str] = []
    context_specs: Dict[str, Dict[str, Any]] = {}
    timestamp_fields: List[str] = []

    _TIMESTAMP_TYPES = frozenset({"DTL", "LDTL", "LDT", "DT", "DATE", "TOD"})

    for sig_name, sig_meta in signals.items():
        base_name = sig_meta["name"].split("[")[0].split(".")[-1]
        sig_type = str(sig_meta.get("type", "")).upper().strip().strip('"')
        kind = sig_meta.get("kind")
        if kind is None:
            kind = "timestamp" if sig_type in _TIMESTAMP_TYPES else (
                "continuous" if sig_type in ("REAL", "LREAL") else "discrete")

        db = sig_meta["db"]
        if includeSubsystems and db not in includeSubsystems:
            continue
        if excludeSubsystems and db in excludeSubsystems:
            continue

        if kind == "timestamp":
            # Reported for time normalisation, never a feature — and not
            # subject to the generic name blacklist (every plant names one
            # field "timestamp"). Explicit excludeSignals still applies.
            if (base_name not in (excludeSignals or [])
                    and sig_name not in (excludeSignals or [])):
                timestamp_fields.append(sig_name)
            continue

        if _excluded(sig_name, base_name):
            continue

        if kind == "continuous" or sig_type in ("REAL", "LREAL"):
            dim = sig_meta.get("dimension") or inferPhysicalDimension(sig_meta.get("unit"))

            if includeDimensions and dim not in includeDimensions:
                continue

            selected_features.append(sig_name)
            feature_dimensions[sig_name] = dim
            feature_subsystems[sig_name] = db
            dimension_groups.setdefault(dim, []).append(sig_name)
            subsystem_groups.setdefault(db, []).append(sig_name)
            continue

        if includeContext:
            categories: List[str] = []
            kind2: Optional[str] = None
            if sig_type == "BOOL":
                kind2, categories = "bool", ["false", "true"]
            elif sig_type in _CONTEXT_INT_TYPES:
                cats = [str(c) for c in (sig_meta.get("enum") or [])]
                if 2 <= len(cats) <= maxContextCardinality:
                    kind2, categories = "enum", cats
            if kind2 is not None:
                context_features.append(sig_name)
                context_specs[sig_name] = {"kind": kind2, "categories": categories}

    num_features = len(selected_features)
    context_dim = sum(
        1 if context_specs[f]["kind"] == "bool" else len(context_specs[f]["categories"]) + 1
        for f in context_features
    )
    input_dim = windowSize * (num_features + context_dim)

    # Calculate optimal network geometry
    if hiddenDim is None:
        hiddenDim = max(32, min(128, int(input_dim // 3)))
    if latentDim is None:
        latentDim = max(8, min(32, int(hiddenDim // 4)))

    model_config = {
        "window_size": windowSize,
        "input_dim": input_dim,
        "num_features": num_features,
        "context_dim": context_dim,
        "num_context": len(context_features),
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
        "contextFeatures": context_features,
        "contextSpecs": context_specs,
        "timestampFields": timestamp_fields,
        "modelConfig": model_config,
        # snake_case backward-compatibility keys
        "feature_dimensions": feature_dimensions,
        "feature_subsystems": feature_subsystems,
        "dimension_groups": dimension_groups,
        "subsystem_groups": subsystem_groups,
        "context_features": context_features,
        "context_specs": context_specs,
        "timestamp_fields": timestamp_fields,
        "model_config": model_config,
    }


def configureModelFromRegistry(
    registry: Dict[str, Any], **kwargs: Any
) -> Dict[str, Any]:
    """Configure from gateway ``GET /registry`` JSON or an embedded WAL schema.

    Same as :func:`configureModelFromSchema` with a registry dict — the
    preferred path (real offsets, resolved enums/aliases, authoritative
    ``dimension``/``min``/``max``/``endianness``). Extra kwargs forward.
    """
    return configureModelFromSchema(registry, **kwargs)


# Backward-compatible snake_case aliases
infer_physical_dimension = inferPhysicalDimension
infer_dimension_from_name = inferDimensionFromName
parse_scl_schema = parseSclSchema
configure_model_from_schema = configureModelFromSchema
configure_model_from_registry = configureModelFromRegistry
signals_from_registry = signalsFromRegistry
