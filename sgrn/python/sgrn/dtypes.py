"""
dtypes.py — S7 type system <-> NumPy dtype bridge.

Lets callers treat gateway registry schemas (``DbSchema`` / ``UdtSchema``)
as NumPy structured dtypes, so a single raw memory read turns straight into
a structured NumPy record instead of a hand-parsed dict. This is the fast
path for feeding PLC memory into ML/data pipelines: see
``Gateway.readDbArray`` and ``DbSchema.toDtype``.

Fixed-width numeric fields use the byte order declared by the schema for
that field. The gateway can mix big-endian and little-endian fields inside
the same DB, so the resulting NumPy dtype must preserve per-field byte
order. Bit-packed BOOL fields, BCD/legacy time types, and variable-length
strings have no native NumPy scalar; those map to raw byte blocks
(``V<n>``) or to the unsigned byte/word that contains them, with helpers
below to unpack the semantic value on demand.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Dict, List, Optional, Sequence, Tuple

import numpy as np

if TYPE_CHECKING:
    from .models import DbField, UdtSchema

__all__ = [
    "S7TypeError",
    "s7ScalarDtype",
    "s7FieldDtype",
    "buildDtype",
    "unpackBool",
    "unpackString",
    "unpackDtl",
    "unpackLdtl",
]


class S7TypeError(ValueError):
    """Raised when an S7 type cannot be represented as a NumPy dtype."""


# Fixed-width S7 types with a direct NumPy equivalent.
# Mirrors sgrn::gateway::twin::kS7TypeDictionaryJson.
_SCALAR_MAP: Dict[str, str] = {
    "BOOL": "u1",  # whole containing byte — use unpackBool() for the bit
    "SINT": "i1",
    "USINT": "u1",
    "BYTE": "u1",
    "CHAR": "S1",
    "WORD": "u2",
    "INT": "i2",
    "UINT": "u2",
    "DWORD": "u4",
    "DINT": "i4",
    "UDINT": "u4",
    "LWORD": "u8",
    "LINT": "i8",
    "ULINT": "u8",
    "REAL": "f4",
    "LREAL": "f8",
    "TIME": "i4",
    "LTIME": "i8",
    "S5TIME": "u2",  # raw BCD duration, not decoded
    "DATE": "u2",  # days since 1990-01-01, not decoded
    "TOD": "u4",
    "LTOD": "u8",
    "LDT": "i8",
    "COUNTER": "u2",  # raw BCD
    "TIMER": "u2",  # raw BCD
}

# Fixed-size BCD/structured time types with no direct numeric scalar.
_OPAQUE_SIZE_BYTES: Dict[str, int] = {
    "DT": 8,
    "DTL": 12,
}

# Variable-length string types: total size = header + n * char_width.
_HEADER_BYTES: Dict[str, int] = {"STRING": 2, "WSTRING": 4, "XSTRING": 8, "XWSTRING": 8}
_CHAR_WIDTH: Dict[str, int] = {"STRING": 1, "WSTRING": 2, "XSTRING": 1, "XWSTRING": 2}


def _resolveByteorder(t_endianness: Optional[str]) -> str:
    if isinstance(t_endianness, str) and t_endianness.lower().startswith("l"):
        return "<"
    return ">"


def s7ScalarDtype(
    t_type_name: str,
    *,
    t_capacity: Optional[int] = None,
    t_endianness: Optional[str] = None,
) -> np.dtype:
    """Map a single S7 leaf type name to a NumPy dtype (no array/count applied)."""
    t = t_type_name.upper()
    if t in _SCALAR_MAP:
        code = _SCALAR_MAP[t]
        if code.startswith("S"):
            return np.dtype(code)
        return np.dtype(f"{_resolveByteorder(t_endianness)}{code}")
    if t in _OPAQUE_SIZE_BYTES:
        return np.dtype(f"V{_OPAQUE_SIZE_BYTES[t]}")
    if t in _HEADER_BYTES:
        n = t_capacity if t_capacity is not None else 0
        size = _HEADER_BYTES[t] + n * _CHAR_WIDTH[t]
        return np.dtype(f"V{max(size, 1)}")
    raise S7TypeError(f"no NumPy mapping for S7 type {t_type_name!r} (STRUCT/UDT fields need build_dtype)")


def s7FieldDtype(
    t_field: "DbField",
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None,
    t_inherited_endianness: Optional[str] = None,
) -> Tuple[np.dtype, tuple]:
    """
    Resolve one ``DbField`` to ``(dtype, shape)`` for use in a structured dtype.

    Handles scalar S7 types, variable-length strings, nested UDTs (via
    ``t_field.udt_name`` + ``t_udts`` lookup, or inline ``t_field.children``), and
    arrays (``t_field.count > 1``).
    """
    shape: tuple = () if t_field.count <= 1 else (t_field.count,)
    resolved_endianness = t_field.endianness or t_inherited_endianness

    if t_field.children:
        sub = _buildDtype(
            t_field.children,
            t_field.struct_size or 0,
            t_udts=t_udts,
            t_inherited_endianness=resolved_endianness,
        )
        return sub, shape

    if t_field.udt_name:
        udt = (t_udts or {}).get(t_field.udt_name)
        if udt is not None:
            sub = _buildDtype(
                udt.fields,
                udt.size_bytes,
                t_udts=t_udts,
                t_inherited_endianness=resolved_endianness,
            )
            return sub, shape
        # UDT referenced but not resolvable here — fall back to an opaque blob.
        size = t_field.struct_size or 1
        return np.dtype(f"V{size}"), shape

    base = s7ScalarDtype(
        t_field.type,
        t_capacity=t_field.capacity,
        t_endianness=resolved_endianness,
    )
    return base, shape


def _buildDtype(
    t_fields: Sequence["DbField"],
    t_itemsize: int,
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None,
    t_inherited_endianness: Optional[str] = None,
) -> np.dtype:
    """
    Build a NumPy structured dtype mirroring a DB/UDT field tree.

    Field offsets come straight from the registry, so the resulting dtype
    maps directly onto the raw bytes returned by ``Gateway.memoryRead`` —
    no per-field parsing required. BOOL fields that share a byte (common
    for PLC bit-packed flags) intentionally overlap in the dtype; use
    :func:`unpackBool` to pull out the individual bit after reading.
    """
    names: List[str] = []
    formats: List[object] = []
    offsets: List[int] = []
    seen: Dict[str, int] = {}

    for f in t_fields:
        dt, shape = s7FieldDtype(f, t_udts=t_udts, t_inherited_endianness=t_inherited_endianness)
        name = f.name
        if name in seen:
            seen[name] += 1
            name = f"{name}__{seen[f.name]}"
        else:
            seen[name] = 0
        names.append(name)
        formats.append((dt, shape) if shape else dt)
        offsets.append(f.offset)

    return np.dtype(
        {"names": names, "formats": formats, "offsets": offsets, "itemsize": max(t_itemsize, 1)},
        align=False,
    )


def buildDtype(
    t_fields: Sequence["DbField"],
    t_itemsize: int,
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None,
) -> np.dtype:
    return _buildDtype(t_fields, t_itemsize, t_udts=t_udts)


# Legacy snake_case alias (the previous ``__all__`` advertised this name).
build_dtype = buildDtype


def unpackBool(t_byte_value: int, t_bit_index: int) -> bool:
    """Extract a single S7 BOOL bit from the byte NumPy read back for its offset."""
    return bool((int(t_byte_value) >> (t_bit_index & 0x7)) & 0x1)


def unpackString(t_raw: bytes, *, t_wide: bool = False) -> str:
    """Decode a Pascal-style S7 STRING/WSTRING byte block (with its header) to ``str``."""
    if not t_wide:
        if len(t_raw) < 2:
            return ""
        cur_len = t_raw[1]
        return bytes(t_raw[2 : 2 + cur_len]).decode("latin-1", errors="replace")
    if len(t_raw) < 4:
        return ""
    cur_len = int.from_bytes(t_raw[2:4], "big")
    return bytes(t_raw[4 : 4 + cur_len * 2]).decode("utf-16-be", errors="replace")


def unpackDtl(t_raw: bytes, *, t_little_endian: bool = False) -> int:
    """Decode a 12-byte S7 DTL struct to integer nanoseconds since the Unix epoch.

    Mirrors ``s7codec::encodeDtl`` (``s7codec/codec.hpp``): year u16 in DB
    byte order, month/day/weekday/hour/min/sec raw bytes, nanoseconds u32
    in DB byte order. PLC wall-clock is interpreted as UTC; callers needing
    local-time semantics must offset explicitly.
    """
    import calendar

    if len(t_raw) != 12:
        raise S7TypeError(f"DTL needs exactly 12 bytes, got {len(t_raw)}")
    order = "little" if t_little_endian else "big"
    year = int.from_bytes(t_raw[0:2], order)
    month, day = t_raw[2], t_raw[3]
    hour, minute, second = t_raw[5], t_raw[6], t_raw[7]
    nanosecond = int.from_bytes(t_raw[8:12], order)
    seconds = calendar.timegm((year, month, day, hour, minute, second))
    return seconds * 1_000_000_000 + nanosecond


def unpackLdtl(t_raw: bytes, *, t_little_endian: bool = False) -> int:
    """Decode an 8-byte S7 LDTL (signed ns since 1970) to integer nanoseconds."""
    if len(t_raw) != 8:
        raise S7TypeError(f"LDTL needs exactly 8 bytes, got {len(t_raw)}")
    return int.from_bytes(t_raw, "little" if t_little_endian else "big", signed=True)


def decodeRecord(
    t_record: Any,
    t_fields: Sequence["DbField"],
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None
) -> Dict[str, Any]:
    """
    Recursively decode a structured NumPy record (from `DbSchema.toDtype()`)
    back into a clean, JSON-serializable Python dictionary.
    
    This is extremely useful for debugging binary WebSocket streams, as it
    automatically unpacks bit-packed BOOLs, Pascal strings, and nested UDTs
    using the registry schema.
    """
    result: Dict[str, Any] = {}
    seen: Dict[str, int] = {}
    
    for f in t_fields:
        name = f.name
        if name in seen:
            seen[name] += 1
            rec_name = f"{name}__{seen[name]}"
        else:
            seen[name] = 0
            rec_name = name
            
        raw_val = t_record[rec_name]
        
        # Handle arrays
        if f.count > 1:
            arr = []
            for i in range(f.count):
                arr.append(_decodeValue(raw_val[i], f, t_udts))
            result[name] = arr
        else:
            result[name] = _decodeValue(raw_val, f, t_udts)
            
    return result


def _decodeValue(t_val: Any, t_field: "DbField", t_udts: Optional[Dict[str, "UdtSchema"]]) -> Any:
    if t_field.children:
        return decodeRecord(t_val, t_field.children, t_udts=t_udts)
    
    if t_field.udt_name:
        udt = (t_udts or {}).get(t_field.udt_name)
        if udt is not None:
            return decodeRecord(t_val, udt.fields, t_udts=t_udts)
        return bytes(t_val).hex()  # Opaque unresolvable UDT blob
        
    t = t_field.type.upper()
    if t == "BOOL":
        return unpackBool(int(t_val), t_field.bit_index)
    elif t in ("STRING", "WSTRING", "XSTRING", "XWSTRING"):
        return unpackString(bytes(t_val), t_wide=("W" in t))
    elif t in ("DTL", "LDTL", "LDT"):
        import datetime as _dt

        little = str(t_field.endianness or "").lower().startswith("l")
        try:
            ns = unpackDtl(bytes(t_val), t_little_endian=little) if t == "DTL" else unpackLdtl(
                bytes(t_val), t_little_endian=little)
        except (S7TypeError, ValueError, OverflowError):
            return None
        return _dt.datetime.fromtimestamp(ns / 1e9, tz=_dt.timezone.utc).isoformat()
    elif "TIME" in t or t == "DATE" or t == "TOD" or "DT" in t:
        if isinstance(t_val, (bytes, memoryview, np.ndarray, np.void)):
            return bytes(t_val).hex()
        return int(t_val)
    else:
        # Standard numeric types
        val = None
        if isinstance(t_val, (np.floating, float)):
            val = float(t_val)
        elif isinstance(t_val, (np.integer, int)):
            val = int(t_val)
        elif isinstance(t_val, (bytes, memoryview, np.void)):
            return bytes(t_val).hex()
        else:
            val = t_val
            
        if t_field.enum_map is not None and isinstance(val, int):
            return t_field.enum_map.get(val, val)
            
        return val
