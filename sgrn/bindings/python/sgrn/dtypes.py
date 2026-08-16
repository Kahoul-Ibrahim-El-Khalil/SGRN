"""
dtypes.py — S7 type system <-> NumPy dtype bridge.

Lets callers treat gateway registry schemas (``DbSchema`` / ``UdtSchema``)
as NumPy structured dtypes, so a single raw memory read turns straight into
a structured NumPy record instead of a hand-parsed dict. This is the fast
path for feeding PLC memory into ML/data pipelines: see
``Gateway.read_db_array`` and ``DbSchema.to_dtype``.

S7 is big-endian on the wire, so every fixed-width numeric field uses the
">" byte-order prefix. Bit-packed BOOL fields, BCD/legacy time types, and
variable-length strings have no native NumPy scalar; those map to raw byte
blocks (``V<n>``) or to the unsigned byte/word that contains them, with
helpers below to unpack the semantic value on demand.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Dict, List, Optional, Sequence, Tuple

import numpy as np

if TYPE_CHECKING:
    from .models import DbField, UdtSchema

__all__ = [
    "S7TypeError",
    "s7_scalar_dtype",
    "s7_field_dtype",
    "build_dtype",
    "unpackBool",
    "unpackString",
]


class S7TypeError(ValueError):
    """Raised when an S7 type cannot be represented as a NumPy dtype."""


# Fixed-width S7 types with a direct NumPy equivalent (big-endian on the wire).
# Mirrors sgrn::gateway::twin::kS7TypeDictionaryJson.
_SCALAR_MAP: Dict[str, str] = {
    "BOOL": ">u1",  # whole containing byte — use unpackBool() for the bit
    "SINT": ">i1",
    "USINT": ">u1",
    "BYTE": ">u1",
    "CHAR": "S1",
    "WORD": ">u2",
    "INT": ">i2",
    "UINT": ">u2",
    "DWORD": ">u4",
    "DINT": ">i4",
    "UDINT": ">u4",
    "LWORD": ">u8",
    "LINT": ">i8",
    "ULINT": ">u8",
    "REAL": ">f4",
    "LREAL": ">f8",
    "TIME": ">i4",
    "LTIME": ">i8",
    "S5TIME": ">u2",  # raw BCD duration, not decoded
    "DATE": ">u2",  # days since 1990-01-01, not decoded
    "TOD": ">u4",
    "LTOD": ">u8",
    "LDT": ">i8",
    "COUNTER": ">u2",  # raw BCD
    "TIMER": ">u2",  # raw BCD
}

# Fixed-size BCD/structured time types with no direct numeric scalar.
_OPAQUE_SIZE_BYTES: Dict[str, int] = {
    "DT": 8,
    "DTL": 12,
}

# Variable-length string types: total size = header + n * char_width.
_HEADER_BYTES: Dict[str, int] = {"STRING": 2, "WSTRING": 4, "XSTRING": 8, "XWSTRING": 8}
_CHAR_WIDTH: Dict[str, int] = {"STRING": 1, "WSTRING": 2, "XSTRING": 1, "XWSTRING": 2}


def s7_scalar_dtype(t_type_name: str, *, t_capacity: Optional[int] = None) -> np.dtype:
    """Map a single S7 leaf type name to a NumPy dtype (no array/count applied)."""
    t = t_type_name.upper()
    if t in _SCALAR_MAP:
        return np.dtype(_SCALAR_MAP[t])
    if t in _OPAQUE_SIZE_BYTES:
        return np.dtype(f"V{_OPAQUE_SIZE_BYTES[t]}")
    if t in _HEADER_BYTES:
        n = t_capacity if t_capacity is not None else 0
        size = _HEADER_BYTES[t] + n * _CHAR_WIDTH[t]
        return np.dtype(f"V{max(size, 1)}")
    raise S7TypeError(f"no NumPy mapping for S7 type {t_type_name!r} (STRUCT/UDT fields need build_dtype)")


def s7_field_dtype(t_field: "DbField", *, t_udts: Optional[Dict[str, "UdtSchema"]] = None) -> Tuple[np.dtype, tuple]:
    """
    Resolve one ``DbField`` to ``(dtype, shape)`` for use in a structured dtype.

    Handles scalar S7 types, variable-length strings, nested UDTs (via
    ``t_field.udt_name`` + ``t_udts`` lookup, or inline ``t_field.children``), and
    arrays (``t_field.count > 1``).
    """
    shape: tuple = () if t_field.count <= 1 else (t_field.count,)

    if t_field.children:
        sub = buildDtype(t_field.children, t_field.struct_size or 0, t_udts=t_udts)
        return sub, shape

    if t_field.udt_name:
        udt = (t_udts or {}).get(t_field.udt_name)
        if udt is not None:
            sub = buildDtype(udt.fields, udt.size_bytes, t_udts=t_udts)
            return sub, shape
        # UDT referenced but not resolvable here — fall back to an opaque blob.
        size = t_field.struct_size or 1
        return np.dtype(f"V{size}"), shape

    base = s7_scalar_dtype(t_field.type, t_capacity=t_field.capacity)
    return base, shape


def buildDtype(
    t_fields: Sequence["DbField"],
    t_itemsize: int,
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None,
) -> np.dtype:
    """
    Build a NumPy structured dtype mirroring a DB/UDT field tree.

    Field offsets come straight from the registry, so the resulting dtype
    maps directly onto the raw bytes returned by ``Gateway.memory_read`` —
    no per-field parsing required. BOOL fields that share a byte (common
    for PLC bit-packed flags) intentionally overlap in the dtype; use
    :func:`unpackBool` to pull out the individual bit after reading.
    """
    names: List[str] = []
    formats: List[object] = []
    offsets: List[int] = []
    seen: Dict[str, int] = {}

    for f in t_fields:
        dt, shape = s7_field_dtype(f, t_udts=t_udts)
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


def decode_record(
    t_record: Any,
    t_fields: Sequence["DbField"],
    *,
    t_udts: Optional[Dict[str, "UdtSchema"]] = None
) -> Dict[str, Any]:
    """
    Recursively decode a structured NumPy record (from `DbSchema.to_dtype()`)
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
                arr.append(_decode_value(raw_val[i], f, t_udts))
            result[name] = arr
        else:
            result[name] = _decode_value(raw_val, f, t_udts)
            
    return result


def _decode_value(t_val: Any, t_field: "DbField", t_udts: Optional[Dict[str, "UdtSchema"]]) -> Any:
    if t_field.children:
        return decode_record(t_val, t_field.children, t_udts=t_udts)
    
    if t_field.udt_name:
        udt = (t_udts or {}).get(t_field.udt_name)
        if udt is not None:
            return decode_record(t_val, udt.fields, t_udts=t_udts)
        return bytes(t_val).hex()  # Opaque unresolvable UDT blob
        
    t = t_field.type.upper()
    if t == "BOOL":
        return unpackBool(int(t_val), t_field.bit_index)
    elif t in ("STRING", "WSTRING", "XSTRING", "XWSTRING"):
        return unpackString(bytes(t_val), t_wide=("W" in t))
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
