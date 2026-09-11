"""
models.py — Typed dataclasses for every shape exposed by the SGRN Gateway API.

These mirror the JSON payloads produced by the gateway (see
``sgrn/gateway/src/adapters/http/*`` and ``SchemaSerializer.cpp``) so callers
get structured, self-documenting objects instead of raw dictionaries.

Every model provides a ``from_dict`` classmethod that is tolerant of missing
fields on the wire (newer servers may add keys; older ones may omit optional
ones).

NumPy integration
------------------
``DbSchema`` / ``UdtSchema`` can turn their field tree into a NumPy
structured dtype via :meth:`DbSchema.to_dtype`, and the raw-memory shapes
(``MemoryReadItem`` / ``MemoryBatchWriteItem`` / ``MemoryBatchWriteResult``)
can view or accept their byte payload as a NumPy array directly — no
intermediate per-field Python parsing needed. See :mod:`sgrn.dtypes` for the
S7 <-> NumPy type mapping.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Union

import numpy as np

from . import dtypes

__all__ = [
    "DbField",
    "DbSchema",
    "UdtSchema",
    "SymbolTag",
    "RegistrySummary",
    "RegistryResponse",
    "ConnectionInfo",
    "SessionInfo",
    "LogEntry",
    "SecurityRule",
    "SecurityPolicyResponse",
    "MemorySpan",
    "MemoryReadItem",
    "MemoryBatchWriteItem",
    "MemoryBatchWriteResult",
    "DataWriteResult",
    "EndpointInfo",
    "to_base64url",
    "as_bytes",
]

# Anything accepted where raw wire bytes are needed: real bytes, or a NumPy
# array/scalar that gets serialized via its native buffer.
BytesLike = Union[bytes, bytearray, memoryview, np.ndarray, np.generic]


def _as_int(t_value: Any, t_default: int = 0) -> int:
    try:
        return int(t_value)
    except (TypeError, ValueError):
        return t_default


def _as_bool(t_value: Any, t_default: bool = False) -> bool:
    if t_value is None:
        return t_default
    if isinstance(t_value, bool):
        return t_value
    return str(t_value).lower() in ("1", "true", "yes", "on")


def _as_str(t_value: Any, t_default: str = "") -> str:
    return t_value if isinstance(t_value, str) else t_default


def _decode_base64url(t_raw: Any) -> bytes:
    """Decode a base64url string into bytes (tolerant of both alphabets)."""
    import base64

    if not isinstance(t_raw, str) or not t_raw:
        return b""
    padded = t_raw + "=" * (-len(t_raw) % 4)
    try:
        return base64.b64decode(padded.replace("-", "+").replace("_", "/"))
    except Exception:
        return b""


def to_base64url(t_data: bytes) -> str:
    """Encode ``t_data`` with the base64url alphabet used by the gateway."""
    import base64

    return base64.b64encode(as_bytes(t_data)).rstrip(b"=").decode("ascii").replace("+", "-").replace("/", "_")


def as_bytes(t_data: BytesLike) -> bytes:
    """
    Normalize anything byte-like — including NumPy arrays/scalars — to
    ``bytes`` for the wire. NumPy values are serialized via their own
    buffer (``.tobytes()``), so an array's dtype byte order is respected
    exactly as constructed (build it with S7's big-endian dtypes from
    :mod:`sgrn.dtypes` to round-trip cleanly).
    """
    if isinstance(t_data, (bytes, bytearray, memoryview)):
        return bytes(t_data)
    if isinstance(t_data, (np.ndarray, np.generic)):
        return np.ascontiguousarray(t_data).tobytes()
    raise TypeError(f"expected bytes-like or numpy data, got {type(t_data).__name__}")


# ── Registry shapes (GET /registry) ───────────────────────────────────────────


@dataclass
class DbField:
    """A single field inside a DB / UDT / struct (leaf or nested container)."""

    name: str
    offset: int
    bit_index: int
    type: str
    count: int = 1
    capacity: Optional[int] = None  # string arrays: nominal element capacity
    udt_name: Optional[str] = None
    struct_size: Optional[int] = None
    unit: Optional[str] = None
    min_val: Optional[float] = None
    max_val: Optional[float] = None
    enum_map: Optional[Dict[int, str]] = None
    endianness: Optional[str] = None  # "big" | "little" (only when non-default)
    trigger_events: bool = False
    is_dynamic: bool = False
    children: List["DbField"] = field(default_factory=list)

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "DbField":
        return cls(
            name=_as_str(t_data.get("name")),
            offset=_as_int(t_data.get("offset")),
            bit_index=_as_int(t_data.get("bit_index")),
            type=_as_str(t_data.get("type")),
            count=_as_int(t_data.get("count"), 1),
            capacity=t_data.get("capacity"),
            udt_name=t_data.get("udt_name"),
            struct_size=_as_int(t_data.get("struct_size")),
            unit=_as_str(t_data.get("unit")),
            min_val=float(t_data["min"]) if "min" in t_data else None,
            max_val=float(t_data["max"]) if "max" in t_data else None,
            enum_map={int(k): str(v) for k, v in t_data.get("enum", {}).items()} if t_data.get("enum") else None,
            endianness=_as_str(t_data.get("endianness")),
            trigger_events=_as_bool(t_data.get("trigger_events")),
            is_dynamic=_as_bool(t_data.get("is_dynamic")),
            children=[
                cls.from_dict(c) for c in t_data.get("children", []) if isinstance(c, dict)
            ],
        )


@dataclass
class DbSchema:
    """Header + optional field tree for one Data Block (DB)."""

    db_number: int
    db_name: str
    size_bytes: int
    source_file: Optional[str] = None
    endianness: Optional[str] = None
    trigger_events: bool = False
    modbus_area: Optional[str] = None  # "holding" | "input" | "coil" | "discrete"
    fields: List[DbField] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._dtype_cache: Optional[np.dtype] = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "DbSchema":
        return cls(
            db_number=_as_int(t_data.get("db_number")),
            db_name=_as_str(t_data.get("db_name")),
            size_bytes=_as_int(t_data.get("size_bytes")),
            source_file=t_data.get("source_file"),
            endianness=t_data.get("endianness"),
            trigger_events=_as_bool(t_data.get("trigger_events")),
            modbus_area=t_data.get("modbus_area"),
            fields=[DbField.from_dict(f) for f in t_data.get("fields", []) if isinstance(f, dict)],
        )

    def to_dtype(self, *, t_udts: Optional[Dict[str, "UdtSchema"]] = None, t_refresh: bool = False) -> np.dtype:
        """
        Build (and cache) a NumPy structured dtype for this DB's field tree.

        Pass ``udts`` (e.g. ``{u.name: u for u in registry.udts}``) so
        fields referencing a UDT by name resolve to their real layout
        instead of an opaque byte blob. See :mod:`sgrn.dtypes`.
        """
        if self._dtype_cache is None or t_refresh:
            self._dtype_cache = dtypes.buildDtype(self.fields, self.size_bytes, t_udts=t_udts)
        return self._dtype_cache


@dataclass
class UdtSchema:
    """User-defined datatype definition (UDT) referenced by DB fields."""

    udt_number: int
    name: str
    size_bytes: int
    fields: List[DbField] = field(default_factory=list)
    endianness: Optional[str] = None
    trigger_events: bool = False

    def __post_init__(self) -> None:
        self._dtype_cache: Optional[np.dtype] = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "UdtSchema":
        return cls(
            udt_number=_as_int(t_data.get("udt_number")),
            name=_as_str(t_data.get("name")),
            size_bytes=_as_int(t_data.get("size_bytes")),
            fields=[DbField.from_dict(f) for f in t_data.get("fields", []) if isinstance(f, dict)],
            endianness=t_data.get("endianness"),
            trigger_events=_as_bool(t_data.get("trigger_events")),
        )

    def to_dtype(self, *, t_udts: Optional[Dict[str, "UdtSchema"]] = None, t_refresh: bool = False) -> np.dtype:
        if self._dtype_cache is None or t_refresh:
            self._dtype_cache = dtypes.buildDtype(self.fields, self.size_bytes, t_udts=t_udts)
        return self._dtype_cache


@dataclass
class SymbolTag:
    """A named symbol/tag from the SCL ``#TAGS`` table."""

    name: str
    table: str
    address: str
    type: str
    remark: Optional[str] = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "SymbolTag":
        return cls(
            name=_as_str(t_data.get("name")),
            table=_as_str(t_data.get("table")),
            address=_as_str(t_data.get("address")),
            type=_as_str(t_data.get("type")),
            remark=t_data.get("remark"),
        )


@dataclass
class RegistrySummary:
    total_dbs: int = 0
    total_udts: int = 0
    total_tags: int = 0
    accessible_dbs: int = 0
    warnings: int = 0
    filtered_db: Optional[int] = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "RegistrySummary":
        return cls(
            total_dbs=_as_int(t_data.get("total_dbs")),
            total_udts=_as_int(t_data.get("total_udts")),
            total_tags=_as_int(t_data.get("total_tags")),
            accessible_dbs=_as_int(t_data.get("accessible_dbs")),
            warnings=_as_int(t_data.get("warnings")),
            filtered_db=t_data.get("filtered_db"),
        )


@dataclass
class RegistryResponse:
    """Top level response of ``GET /registry``."""

    dbs: List[DbSchema] = field(default_factory=list)
    udts: List[UdtSchema] = field(default_factory=list)
    tags: List[SymbolTag] = field(default_factory=list)
    summary: Optional[RegistrySummary] = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "RegistryResponse":
        return cls(
            dbs=[DbSchema.from_dict(d) for d in t_data.get("dbs", []) if isinstance(d, dict)],
            udts=[UdtSchema.from_dict(u) for u in t_data.get("udts", []) if isinstance(u, dict)],
            tags=[SymbolTag.from_dict(t) for t in t_data.get("tags", []) if isinstance(t, dict)],
            summary=RegistrySummary.from_dict(t_data["summary"])
            if isinstance(t_data.get("summary"), dict)
            else None,
        )

    def db_by_name(self, t_name: str) -> Optional["DbSchema"]:
        for db in self.dbs:
            if db.db_name == t_name:
                return db
        return None

    def db_by_number(self, t_number: int) -> Optional["DbSchema"]:
        for db in self.dbs:
            if db.db_number == t_number:
                return db
        return None

    def udts_by_name(self) -> Dict[str, UdtSchema]:
        """Convenience lookup for ``DbSchema.to_dtype(udts=...)``."""
        return {u.name: u for u in self.udts}


# ── Diagnostics shapes ─────────────────────────────────────────────────────────


@dataclass
class ConnectionInfo:
    """An active / recent north- or south-bound connection (``GET /connections``)."""

    type: str
    ip: str
    endpoint: str
    first_seen: int = 0
    last_seen: int = 0
    event_count: int = 0

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "ConnectionInfo":
        return cls(
            type=_as_str(t_data.get("type")),
            ip=_as_str(t_data.get("ip")),
            endpoint=_as_str(t_data.get("endpoint")),
            first_seen=_as_int(t_data.get("first_seen")),
            last_seen=_as_int(t_data.get("last_seen")),
            event_count=_as_int(t_data.get("event_count")),
        )


@dataclass
class SessionInfo:
    """A client session (``GET /db/sessions``)."""

    id: Any = None
    ip: str = ""
    connect_time: int = 0
    bytes_sent: int = 0
    bytes_received: int = 0

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "SessionInfo":
        return cls(
            id=t_data.get("id"),
            ip=_as_str(t_data.get("ip")),
            connect_time=_as_int(t_data.get("connect_time")),
            bytes_sent=_as_int(t_data.get("bytes_sent")),
            bytes_received=_as_int(t_data.get("bytes_received")),
        )


@dataclass
class LogEntry:
    """A system log line (``GET /db/logs``)."""

    ts: Any = None
    level: str = ""
    msg: str = ""

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "LogEntry":
        return cls(
            ts=t_data.get("ts"),
            level=_as_str(t_data.get("level")),
            msg=_as_str(t_data.get("msg")),
        )


# ── Security policy shapes (GET /api/policy) ──────────────────────────────────


@dataclass
class SecurityRule:
    """One evaluated security rule."""

    protocol: str = ""
    action: str = ""  # "ALLOW" | "DENY"
    specificity: int = 0
    cidrs: List[str] = field(default_factory=list)
    dbs: List[int] = field(default_factory=list)
    any_db: bool = False
    origins: List[str] = field(default_factory=list)
    headers: List[str] = field(default_factory=list)
    sessions: List[str] = field(default_factory=list)

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "SecurityRule":
        return cls(
            protocol=_as_str(t_data.get("protocol")),
            action=_as_str(t_data.get("action")),
            specificity=_as_int(t_data.get("specificity")),
            cidrs=[c for c in t_data.get("cidrs", []) if isinstance(c, str)],
            dbs=[_as_int(d) for d in t_data.get("dbs", [])],
            any_db=_as_bool(t_data.get("any_db")),
            origins=[o for o in t_data.get("origins", []) if isinstance(o, str)],
            headers=[h for h in t_data.get("headers", []) if isinstance(h, str)],
            sessions=[s for s in t_data.get("sessions", []) if isinstance(s, str)],
        )


@dataclass
class SecurityPolicyResponse:
    """Active policy as reported by ``GET /api/policy``."""

    rules: List[SecurityRule] = field(default_factory=list)
    total: int = 0
    mode: str = "relaxed"

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "SecurityPolicyResponse":
        return cls(
            rules=[SecurityRule.from_dict(r) for r in t_data.get("rules", []) if isinstance(r, dict)],
            total=_as_int(t_data.get("total")),
            mode=_as_str(t_data.get("mode"), "relaxed"),
        )


# ── Raw memory shapes (GET/PUT /memory/*) ─────────────────────────────────────


@dataclass
class MemorySpan:
    """Read request triplet (db, offset, size)."""

    db: int
    offset: int
    size: int


@dataclass
class MemoryReadItem:
    """One read result of ``GET /memory/batch``."""

    db: int
    offset: int
    size: int
    data: bytes = b""

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "MemoryReadItem":
        return cls(
            db=_as_int(t_data.get("db")),
            offset=_as_int(t_data.get("offset")),
            size=_as_int(t_data.get("size")),
            data=_decode_base64url(t_data.get("data")),
        )

    def as_array(self, t_dtype: Any = np.uint8) -> np.ndarray:
        """
        View this item's raw bytes as a NumPy array.

        Pass an S7-aware dtype (e.g. ``sgrn.dtypes.s7_scalar_dtype("REAL")``
        or a structured dtype from ``DbSchema.to_dtype``) to interpret the
        bytes semantically instead of as a flat ``uint8`` buffer.
        """
        return np.frombuffer(self.data, dtype=t_dtype).copy()


@dataclass
class MemoryBatchWriteItem:
    """
    One item of ``PUT /memory/batch``.

    ``data`` accepts raw ``bytes`` or a NumPy array/scalar (e.g. built from
    an S7 dtype via :mod:`sgrn.dtypes`); it is normalized to bytes for the
    wire on send.
    """

    db: int
    offset: int
    data: BytesLike
    size: Optional[int] = None  # derived from ``len(data)`` when not given


@dataclass
class MemoryBatchWriteResult:
    """One item of the ``PUT /memory/batch`` response."""

    db: int
    offset: int
    size: int
    written: bytes = b""

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "MemoryBatchWriteResult":
        return cls(
            db=_as_int(t_data.get("db")),
            offset=_as_int(t_data.get("offset")),
            size=_as_int(t_data.get("size")),
            written=_decode_base64url(t_data.get("written")),
        )

    def as_array(self, t_dtype: Any = np.uint8) -> np.ndarray:
        return np.frombuffer(self.written, dtype=t_dtype).copy()


# ── Data write shapes (POST /data/...) ────────────────────────────────────────


@dataclass
class DataWriteResult:
    """Response of ``POST /data/<path>`` or ``POST /data/<path>/<N>``."""

    db: int = 0
    path: str = ""
    fields_written: int = 0
    index: Optional[int] = None
    value: Any = None

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "DataWriteResult":
        return cls(
            db=_as_int(t_data.get("db")),
            path=_as_str(t_data.get("path")),
            fields_written=_as_int(t_data.get("fields_written")),
            index=t_data.get("index"),
            value=t_data.get("value"),
        )


@dataclass
class EndpointInfo:
    """A documented API endpoint (``GET /endpoints``)."""

    path: str
    method: str
    description: str

    @classmethod
    def from_dict(cls, t_data: Dict[str, Any]) -> "EndpointInfo":
        return cls(
            path=_as_str(t_data.get("path")),
            method=_as_str(t_data.get("method")),
            description=_as_str(t_data.get("description")),
        )
