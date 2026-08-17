#!/usr/bin/env python3
"""
Live binary/JSON parity checks for a running SGRN gateway.

This module can run in two modes:

- Managed mode, the default, starts the gateway and simulation itself via
  ``demo.py`` and tears them down afterward.
- External mode, enabled with ``SGRN_LIVE_EXTERNAL=1``, assumes a gateway is
  already running and only connects to it.

The tests compare:

- WebSocket JSON seed frames against the registry-backed semantic view
- WebSocket binary frames against HTTP raw memory bytes
- WebSocket binary decoding against the registry-backed NumPy decode path

Visualization helpers:

- ``SGRN_SHOW_PAYLOADS=1`` prints a compact payload dump
- ``SGRN_CAPTURE_DIR=/tmp/sgrn-captures`` writes JSON snapshots to disk
- ``SGRN_LIVE_SIMULATION=<name or index>`` selects the simulation to launch
"""

from __future__ import annotations

import asyncio
import copy
import argparse
import json
import logging
import os
import re
import subprocess
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

import numpy as np

ROOT_DIR = Path(__file__).resolve().parents[1]
PY_BINDINGS_DIR = ROOT_DIR / "sgrn" / "bindings" / "python"
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))
if str(PY_BINDINGS_DIR) not in sys.path:
    sys.path.insert(0, str(PY_BINDINGS_DIR))

import demo  # noqa: E402
from sgrn.dtypes import decode_record  # noqa: E402
from sgrn.gateway import Gateway, GatewayError  # noqa: E402
from sgrn.models import DbField, DbSchema  # noqa: E402
from sgrn.telemetry import GatewayTelemetry  # noqa: E402

log = logging.getLogger("sgrn.live_binary")

DEFAULT_HTTP_URL = os.environ.get("SGRN_LIVE_GATEWAY_URL", "http://localhost:8000")
DEFAULT_WS_URL = os.environ.get("SGRN_LIVE_WS_URL", "ws://localhost:8001")
SHOW_PAYLOADS = os.environ.get("SGRN_SHOW_PAYLOADS", "").strip().lower() in {"1", "true", "yes", "on"}
CAPTURE_DIR = os.environ.get("SGRN_CAPTURE_DIR")
LIVE_TIMEOUT = float(os.environ.get("SGRN_LIVE_TIMEOUT", "10"))
MANAGED_LIVE = os.environ.get("SGRN_LIVE_EXTERNAL", "").strip().lower() not in {"1", "true", "yes", "on"}
SIMULATION_CHOICE = os.environ.get("SGRN_LIVE_SIMULATION")


@dataclass
class LiveContext:
    run: Optional[demo.DemoRun]
    gateway: Gateway
    registry: Any
    schema: DbSchema
    field: DbField
    db_dtype: np.dtype
    field_dtype: np.dtype

    def stop(self) -> None:
        if self.run is not None:
            demo.cleanupProcesses(self.run.gateway_proc, self.run.shell_proc)


_CTX: Optional[LiveContext] = None


def _first_db(registry) -> DbSchema:
    for db in registry.dbs:
        if db.size_bytes > 0 and db.fields:
            return db
    raise AssertionError("registry contains no DB with a non-zero fielded size")


def _first_field(schema: DbSchema) -> DbField:
    if not schema.fields:
        raise AssertionError(f"DB {schema.db_name} has no fields")
    return schema.fields[0]


def _field_at_offset_zero(field: DbField) -> DbField:
    temp_field = copy.deepcopy(field)
    temp_field.offset = 0
    return temp_field


def _field_dtype(schema: DbSchema, field: DbField, reg_udts: Dict[str, Any]) -> np.dtype:
    temp_schema = DbSchema(
        db_number=schema.db_number,
        db_name=schema.db_name,
        size_bytes=max(1, field_struct_size(schema, field, reg_udts)),
        fields=[_field_at_offset_zero(field)],
    )
    return temp_schema.to_dtype(t_udts=reg_udts)


def field_struct_size(schema: DbSchema, field: DbField, reg_udts: Dict[str, Any]) -> int:
    dtype = schema.to_dtype(t_udts=reg_udts)
    if field.name not in dtype.fields:
        raise AssertionError(f"field {field.name!r} not present in dtype for DB {schema.db_name}")
    return int(dtype.fields[field.name][0].itemsize)


def _hex_preview(data: bytes, limit: int = 32) -> str:
    chunk = data[:limit]
    return " ".join(f"{b:02X}" for b in chunk) + (" ..." if len(data) > limit else "")


def _normalize(value: Any) -> Any:
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, np.ndarray):
        return value.tolist()
    return value


def _compare_values(expected: Any, actual: Any, path: str = "") -> None:
    expected = _normalize(expected)
    actual = _normalize(actual)

    if isinstance(expected, dict) and isinstance(actual, dict):
        assert expected.keys() == actual.keys(), f"{path}: keys differ: {expected.keys()} != {actual.keys()}"
        for key in expected:
            child = f"{path}.{key}" if path else key
            _compare_values(expected[key], actual[key], child)
        return

    if isinstance(expected, (list, tuple)) and isinstance(actual, (list, tuple)):
        assert len(expected) == len(actual), f"{path}: length differs: {len(expected)} != {len(actual)}"
        for idx, (ev, av) in enumerate(zip(expected, actual)):
            _compare_values(ev, av, f"{path}[{idx}]")
        return

    if isinstance(expected, float) or isinstance(actual, float):
        assert np.isclose(float(expected), float(actual), rtol=1e-6, atol=1e-6), f"{path}: {expected} != {actual}"
        return

    assert expected == actual, f"{path}: {expected!r} != {actual!r}"


def _capture_summary(name: str, payload: Dict[str, Any]) -> None:
    if not CAPTURE_DIR:
        return
    out_dir = Path(CAPTURE_DIR)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"{name}.json").write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def _print_snapshot(title: str, *, db_name: str, db_num: int, timestamp: float, raw: bytes, decoded: Any, semantic: Any) -> None:
    print(f"\n=== {title} ===")
    print(f"DB: {db_name} ({db_num})")
    print(f"Timestamp: {timestamp:.3f}s")
    print(f"Bytes: {len(raw)}")
    print(f"Raw hex: {_hex_preview(raw)}")
    print("Decoded:")
    print(json.dumps(decoded, indent=2, sort_keys=True, default=str))
    print("Semantic:")
    print(json.dumps(semantic, indent=2, sort_keys=True, default=str))


_HEX_DTL_RE = re.compile(r"^[0-9a-fA-F]{24}$")


def _dtl_hex_to_string(raw_hex: str) -> str:
    raw = bytes.fromhex(raw_hex)
    if len(raw) != 12:
        return raw_hex
    year, month, day, _, hour, minute, second, nanosecond = struct.unpack(">HBBBBBBI", raw)
    return f"{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}.{nanosecond:09d}"


def _normalize_dtl_value(value: Any) -> Any:
    if isinstance(value, str) and _HEX_DTL_RE.fullmatch(value):
        return _dtl_hex_to_string(value)
    return value


def _normalize_tree_for_schema(value: Any, fields: list[DbField], udts: Dict[str, Any]) -> Any:
    if not isinstance(value, dict):
        return _normalize_dtl_value(value)

    result: Dict[str, Any] = {}
    seen: Dict[str, int] = {}
    for field in fields:
        key = field.name
        if key in seen:
            seen[key] += 1
            value_key = f"{key}__{seen[key]}"
        else:
            seen[key] = 0
            value_key = key

        if value_key not in value:
            continue

        raw_value = value[value_key]
        field_type = field.type.upper()

        if field.count > 1 and isinstance(raw_value, list):
            result[key] = [
                _normalize_tree_for_schema(item, field.children, udts) if field.children else _normalize_dtl_value(item)
                for item in raw_value
            ]
            continue

        if field.children:
            result[key] = _normalize_tree_for_schema(raw_value, field.children, udts)
            continue

        if field.udt_name and field.udt_name in udts and isinstance(raw_value, dict):
            result[key] = _normalize_tree_for_schema(raw_value, udts[field.udt_name].fields, udts)
            continue

        if field_type in {"DTL", "DATETIME"}:
            result[key] = _normalize_dtl_value(raw_value)
            continue

        result[key] = raw_value

    for key, raw_value in value.items():
        result.setdefault(key, _normalize_dtl_value(raw_value))
    return result


def _schema_summary(schema: DbSchema) -> str:
    return f"DB{schema.db_number} {schema.db_name} ({schema.size_bytes} bytes)"


def _render_side_by_side(columns: list[tuple[str, Any]], width: int = 52) -> str:
    rendered = []
    for title, value in columns:
        text = json.dumps(value, indent=2, sort_keys=True, default=str)
        rendered.append((title, text.splitlines() or ["{}"]))

    header = " | ".join(f"{title:<{width}}" for title, _ in rendered)
    separator = "-+-".join("-" * width for _ in rendered)
    lines = [header, separator]
    max_lines = max(len(chunk) for _, chunk in rendered)
    for idx in range(max_lines):
        row = []
        for _, chunk in rendered:
            cell = chunk[idx] if idx < len(chunk) else ""
            if len(cell) > width:
                cell = cell[: width - 3] + "..."
            row.append(f"{cell:<{width}}")
        lines.append(" | ".join(row))
    return "\n".join(lines)


def _comparison_artifact(title: str, schema: DbSchema, columns: list[tuple[str, Any]], *, notes: Optional[list[str]] = None) -> str:
    lines = [f"=== {title} ===", f"Schema: {_schema_summary(schema)}"]
    if notes:
        lines.extend(f"Note: {note}" for note in notes)
    lines.append(_render_side_by_side(columns))
    return "\n".join(lines)


def _report_or_raise(title: str, schema: DbSchema, columns: list[tuple[str, Any]], exc: Optional[AssertionError] = None, *, notes: Optional[list[str]] = None) -> None:
    artifact = _comparison_artifact(title, schema, columns, notes=notes)
    print(artifact)
    if exc is not None:
        raise AssertionError(f"{exc}\n\n{artifact}") from exc


def _wait_for_registry(gateway: Gateway, timeout: float = 30.0) -> Any:
    deadline = time.monotonic() + timeout
    last_error: Optional[Exception] = None
    while time.monotonic() < deadline:
        try:
            return gateway.registry()
        except GatewayError as exc:
            last_error = exc
            time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for gateway registry: {last_error}")


def _setup_context() -> LiveContext:
    run = None
    try:
        if MANAGED_LIVE:
            if os.geteuid() != 0:
                raise RuntimeError("managed live tests require root because demo.py launches s7shell on privileged ports")
            selected = demo.resolve_simulation(SIMULATION_CHOICE)
            run = demo.start_simulation(
                selected,
                open_dashboard=False,
                gateway_stdout=subprocess.DEVNULL,
                gateway_stderr=subprocess.DEVNULL,
                shell_stdout=subprocess.DEVNULL,
                shell_stderr=subprocess.DEVNULL,
            )
            if run.shell_proc.poll() is not None:
                raise RuntimeError(
                    "s7shell exited before the test could freeze the simulation; "
                    "fix the simulation scripts first (the managed live test will not continue against a failed compile)."
                )

        gateway = Gateway(DEFAULT_HTTP_URL, t_ws_url=DEFAULT_WS_URL)
        registry = _wait_for_registry(gateway)
        schema = _first_db(registry)
        field = _first_field(schema)

        if run is not None:
            demo.stop_shell(run.shell_proc)
            time.sleep(0.5)

        db_dtype = schema.to_dtype(t_udts=registry.udts_by_name())
        field_dtype = _field_dtype(schema, field, registry.udts_by_name())
        return LiveContext(run=run, gateway=gateway, registry=registry, schema=schema, field=field, db_dtype=db_dtype, field_dtype=field_dtype)
    except Exception:
        if run is not None:
            demo.cleanupProcesses(run.gateway_proc, run.shell_proc)
        raise


def setup_module(module) -> None:  # noqa: D401 - pytest hook
    global _CTX
    _CTX = _setup_context()


def teardown_module(module) -> None:  # noqa: D401 - pytest hook
    global _CTX
    if _CTX is not None:
        _CTX.stop()
        _CTX = None


async def _wait_for_event(event: asyncio.Event, timeout: float) -> None:
    try:
        await asyncio.wait_for(event.wait(), timeout=timeout)
    except asyncio.TimeoutError as exc:
        raise AssertionError(f"timed out waiting for websocket payload after {timeout}s") from exc


async def _capture_json_snapshot(ctx: LiveContext) -> Dict[str, Any]:
    json_event = asyncio.Event()
    captured_json: Dict[str, Any] = {}

    def on_json(frame: Dict[str, Any]) -> None:
        captured_json.clear()
        captured_json.update(frame)
        json_event.set()

    telemetry = GatewayTelemetry(
        DEFAULT_WS_URL,
        t_on_message=on_json,
        t_open_timeout=5.0,
        t_reconnect_seconds=5.0,
    )
    telemetry.subscribe(ctx.schema.db_name)
    telemetry.start()

    try:
        await _wait_for_event(json_event, LIVE_TIMEOUT)
    finally:
        await telemetry.stop()

    assert ctx.schema.db_name in captured_json, f"websocket JSON snapshot did not include DB {ctx.schema.db_name!r}"
    return captured_json


async def _capture_binary_full_db_snapshot(ctx: LiveContext) -> Tuple[int, float, np.void]:
    binary_event = asyncio.Event()
    captured_meta: Dict[str, Any] = {}

    def on_binary(db: int, ts: float, record: np.void) -> None:
        captured_meta["db"] = db
        captured_meta["ts"] = ts
        captured_meta["record"] = record
        binary_event.set()

    telemetry = GatewayTelemetry(
        DEFAULT_WS_URL,
        t_on_binary=on_binary,
        t_binary_dtypes={ctx.schema.db_number: ctx.db_dtype},
        t_open_timeout=5.0,
        t_reconnect_seconds=5.0,
    )
    telemetry.subscribe_binary(ctx.schema.db_number)
    telemetry.start()

    try:
        await _wait_for_event(binary_event, LIVE_TIMEOUT)
    finally:
        await telemetry.stop()

    return int(captured_meta["db"]), float(captured_meta["ts"]), captured_meta["record"]


async def _capture_binary_field_slice(ctx: LiveContext) -> Tuple[int, float, np.void]:
    field_size = field_struct_size(ctx.schema, ctx.field, ctx.registry.udts_by_name())
    binary_event = asyncio.Event()
    captured_meta: Dict[str, Any] = {}

    def on_binary(db: int, ts: float, record: np.void) -> None:
        captured_meta["db"] = db
        captured_meta["ts"] = ts
        captured_meta["record"] = record
        binary_event.set()

    telemetry = GatewayTelemetry(
        DEFAULT_WS_URL,
        t_on_binary=on_binary,
        t_binary_dtypes={ctx.schema.db_number: ctx.field_dtype},
        t_open_timeout=5.0,
        t_reconnect_seconds=5.0,
    )
    telemetry.subscribe_binary(ctx.schema.db_number, t_offset=ctx.field.offset, t_size=field_size)
    telemetry.start()

    try:
        await _wait_for_event(binary_event, LIVE_TIMEOUT)
    finally:
        await telemetry.stop()

    return int(captured_meta["db"]), float(captured_meta["ts"]), captured_meta["record"]


def _db_json_from_snapshot(snapshot: Dict[str, Any], schema: DbSchema) -> Any:
    if schema.db_name in snapshot:
        return snapshot[schema.db_name]
    if len(snapshot) == 1:
        return next(iter(snapshot.values()))
    raise AssertionError(f"snapshot does not contain DB {schema.db_name!r}")


def test_live_websocket_json_seed_matches_http_db_view() -> None:
    assert _CTX is not None
    ws_json = asyncio.run(_capture_json_snapshot(_CTX))
    http_record = _CTX.gateway.read_db_array(_CTX.schema.db_name, t_registry=_CTX.registry)
    http_semantic = decode_record(http_record, _CTX.schema.fields, t_udts=_CTX.registry.udts_by_name())
    ws_db_json = _db_json_from_snapshot(ws_json, _CTX.schema)
    http_semantic = _normalize_tree_for_schema(http_semantic, _CTX.schema.fields, _CTX.registry.udts_by_name())
    ws_db_json = _normalize_tree_for_schema(ws_db_json, _CTX.schema.fields, _CTX.registry.udts_by_name())
    try:
        _compare_values(http_semantic, ws_db_json, _CTX.schema.db_name)
    except AssertionError as exc:
        _report_or_raise(
            "JSON seed vs HTTP semantic",
            _CTX.schema,
            [
                ("HTTP semantic", http_semantic),
                ("WebSocket JSON", ws_db_json),
            ],
            exc,
        )
    if SHOW_PAYLOADS:
        _report_or_raise(
            "JSON seed vs HTTP semantic",
            _CTX.schema,
            [
                ("HTTP semantic", http_semantic),
                ("WebSocket JSON", ws_db_json),
            ],
        )
    _capture_summary(
        f"{_CTX.schema.db_name}_json_seed",
        {"http_semantic": http_semantic, "websocket_json": ws_db_json},
    )


def test_live_binary_full_db_matches_http_memory_and_json_view() -> None:
    assert _CTX is not None
    ws_json = asyncio.run(_capture_json_snapshot(_CTX))
    db_num, ts, record = asyncio.run(_capture_binary_full_db_snapshot(_CTX))
    http_record = _CTX.gateway.read_db_array(_CTX.schema.db_name, t_registry=_CTX.registry)
    http_raw = _CTX.gateway.memory_read(_CTX.schema.db_number, 0, _CTX.schema.size_bytes)
    decoded = decode_record(record, _CTX.schema.fields, t_udts=_CTX.registry.udts_by_name())
    semantic = decode_record(http_record, _CTX.schema.fields, t_udts=_CTX.registry.udts_by_name())
    decoded = _normalize_tree_for_schema(decoded, _CTX.schema.fields, _CTX.registry.udts_by_name())
    ws_db_json = _normalize_tree_for_schema(_db_json_from_snapshot(ws_json, _CTX.schema), _CTX.schema.fields, _CTX.registry.udts_by_name())
    semantic = _normalize_tree_for_schema(semantic, _CTX.schema.fields, _CTX.registry.udts_by_name())

    artifacts = [
        ("HTTP semantic", semantic),
        ("WebSocket JSON", ws_db_json),
        ("WebSocket binary", decoded),
    ]

    try:
        assert db_num == _CTX.schema.db_number
        assert record.tobytes() == http_raw
        assert record.tobytes() == http_record.tobytes()
        _compare_values(semantic, decoded, _CTX.schema.db_name)
        _compare_values(ws_db_json, semantic, _CTX.schema.db_name)
    except AssertionError as exc:
        _report_or_raise("Full DB binary vs JSON vs HTTP", _CTX.schema, artifacts, exc, notes=[f"timestamp={ts:.3f}s", f"db={db_num}"])
    if SHOW_PAYLOADS:
        _report_or_raise("Full DB binary vs JSON vs HTTP", _CTX.schema, artifacts, notes=[f"timestamp={ts:.3f}s", f"db={db_num}"])

    _capture_summary(
        f"{_CTX.schema.db_name}_binary_full",
        {
            "db": _CTX.schema.db_number,
            "timestamp_s": ts,
            "raw_hex": http_raw.hex(),
            "decoded": decoded,
            "semantic": semantic,
        },
    )


def test_live_binary_field_slice_matches_http_json_field_view() -> None:
    assert _CTX is not None
    ws_json = asyncio.run(_capture_json_snapshot(_CTX))
    db_num, ts, record = asyncio.run(_capture_binary_field_slice(_CTX))
    field_size = field_struct_size(_CTX.schema, _CTX.field, _CTX.registry.udts_by_name())
    http_raw = _CTX.gateway.memory_read(_CTX.schema.db_number, _CTX.field.offset, field_size)
    http_json = _CTX.gateway.read_data(f"{_CTX.schema.db_name}/{_CTX.field.name}")
    decoded = decode_record(record, [_field_at_offset_zero(_CTX.field)], t_udts=_CTX.registry.udts_by_name())
    decoded_value = decoded[_CTX.field.name]
    http_json = _normalize_dtl_value(http_json)
    decoded_value = _normalize_dtl_value(decoded_value)

    _compare_values(http_json, decoded_value, f"{_CTX.schema.db_name}/{_CTX.field.name}")
    assert record.tobytes() == http_raw
    assert db_num == _CTX.schema.db_number
    field_ws_json = _normalize_tree_for_schema(
        _db_json_from_snapshot(ws_json, _CTX.schema), _CTX.schema.fields, _CTX.registry.udts_by_name()
    )
    full_http_json = _normalize_tree_for_schema(
        _CTX.gateway.read_data(_CTX.schema.db_name), _CTX.schema.fields, _CTX.registry.udts_by_name()
    )
    try:
        _compare_values(field_ws_json, full_http_json, _CTX.schema.db_name)
    except AssertionError as exc:
        _report_or_raise(
            "Field slice binary vs JSON vs HTTP",
            _CTX.schema,
            [
                (f"HTTP field { _CTX.field.name }", http_json),
                ("WebSocket JSON", field_ws_json),
                ("WebSocket binary", decoded_value),
            ],
            exc,
            notes=[f"field={_CTX.field.name}", f"offset={_CTX.field.offset}", f"size={field_size}", f"timestamp={ts:.3f}s"],
        )
    if SHOW_PAYLOADS:
        _report_or_raise(
            "Field slice binary vs JSON vs HTTP",
            _CTX.schema,
            [
                (f"HTTP field {_CTX.field.name}", http_json),
                ("WebSocket JSON", field_ws_json),
                ("WebSocket binary", decoded_value),
            ],
            notes=[f"field={_CTX.field.name}", f"offset={_CTX.field.offset}", f"size={field_size}", f"timestamp={ts:.3f}s"],
        )

    _capture_summary(
        f"{_CTX.schema.db_name}_{_CTX.field.name}_field_slice",
        {
            "db": _CTX.schema.db_number,
            "field": _CTX.field.name,
            "offset": _CTX.field.offset,
            "size": field_size,
            "timestamp_s": ts,
            "raw_hex": http_raw.hex(),
            "decoded": decoded_value,
            "semantic": http_json,
        },
    )


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
    try:
        setup_module(None)
        try:
            test_live_websocket_json_seed_matches_http_db_view()
            test_live_binary_full_db_matches_http_memory_and_json_view()
            test_live_binary_field_slice_matches_http_json_field_view()
            print("Live binary snapshot tests completed.")
        finally:
            teardown_module(None)
    finally:
        if _CTX is not None:
            teardown_module(None)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run live binary/JSON snapshot checks against a SGRN gateway.")
    parser.add_argument(
        "--simulation",
        dest="simulation",
        help="Simulation name or 1-based index to launch in managed mode.",
    )
    args = parser.parse_args()
    if args.simulation:
        SIMULATION_CHOICE = args.simulation
    if MANAGED_LIVE and os.geteuid() != 0:
        os.execvp("sudo", ["sudo", "-E", sys.executable, os.path.abspath(__file__), *sys.argv[1:]])
    main()
