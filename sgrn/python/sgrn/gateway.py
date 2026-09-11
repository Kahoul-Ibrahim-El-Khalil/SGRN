"""
gateway.py — Python bindings for the SGRN Gateway northbound REST API.

Covers the complete currently-exposed HTTP surface:

    GET  /registry               schema directory (all DBs, UDTs, tags)
    GET  /registry?headers=true  DB headers only (fast)
    GET  /registry?db=<N>        single DB schema
    GET  /registry/types         S7 type dictionary
    GET  /registry/modbus        Modbus virtual register map
    GET  /data[/<path>[/<N>]]    full twin / DB / field / array element
    POST /data[/<path>[/<N>]]    merge write (object) or scalar write (leaf/array)
    PUT  /data/<path>            full field replacement
    GET  /memory/db/<db>/offset/<o>/size/<s>    raw bytes read
    PUT  /memory/db/<db>/offset/<o>/size/<s>    raw bytes write
    PUT  /memory/batch           atomic multi-DB write (base64url JSON)
    GET  /connections /db/history /db/sessions /db/logs /endpoints
    GET  /api/policy             live security policy

The HTTP client itself stays dependency-free (``urllib.request``) and returns
typed :mod:`sgrn.models` dataclasses for known shapes.

NumPy is a first-class citizen on the raw-memory surface: ``memory_read`` /
``memory_batch_read`` can hand back NumPy arrays directly, ``memory_write`` /
``memory_batch_write`` accept them, and :meth:`Gateway.read_db_array` turns a
registry schema + one memory read into a single structured NumPy record —
no manual byte parsing required. See :mod:`sgrn.dtypes`.
"""

from __future__ import annotations

import json
import ssl
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Union

import numpy as np

from . import dtypes
from .models import (
    BytesLike,
    ConnectionInfo,
    DataWriteResult,
    EndpointInfo,
    LogEntry,
    MemoryBatchWriteItem,
    MemoryBatchWriteResult,
    MemoryReadItem,
    MemorySpan,
    RegistryResponse,
    SecurityPolicyResponse,
    SessionInfo,
    as_bytes,
    to_base64url,
)

__all__ = [
    "Gateway",
    "GatewayClient",
    "GatewayError",
    "GatewayHTTPError",
]


class GatewayError(Exception):
    """Base error for all binding-level failures."""


class GatewayHTTPError(GatewayError):
    """The gateway replied with a non-success HTTP status code."""

    def __init__(self, t_status: int, t_url: str, t_body: Any = None):
        self.status = t_status
        self.url = t_url
        self.body = t_body
        message = f"HTTP {t_status} from {t_url}"
        if isinstance(t_body, dict) and t_body.get("error"):
            message += f": {t_body['error']}"
        super().__init__(message)


class Gateway:
    """
    Synchronous HTTP client for the SGRN Gateway.

    Example
    -------
    >>> gw = Gateway("http://localhost:8000")
    >>> reg = gw.registry(headers_only=True)
    >>> for db in reg.dbs:
    ...     print(db.db_number, db.db_name)

    >>> # Structured NumPy access to a whole DB in one round trip:
    >>> record = gw.read_db_array("ReactorCore")
    >>> record["thermal_power_mw"]
    """

    _DEFAULT_WS_PORT_SHIFT = 1  # sample config: http 8000 → websocket 8001

    def __init__(
        self,
        t_base_url: str = "http://localhost:8000",
        *,
        t_ws_url: Optional[str] = None,
        t_timeout: float = 15.0,
        t_headers: Optional[Dict[str, str]] = None,
        t_verify: Union[bool, str] = True,
        t_origin: Optional[str] = None,
    ) -> None:
        self.base_url = t_base_url.rstrip("/")
        self.timeout = max(0.0, t_timeout)
        self.headers: Dict[str, str] = dict(t_headers or {})
        if t_origin:
            self.headers.setdefault("Origin", t_origin)
        self._verify = t_verify
        self.ws_url = t_ws_url or self._default_ws_url()
        self._registry_cache: Optional[RegistryResponse] = None

    # ── Internals ─────────────────────────────────────────────────────────────

    def _default_ws_url(self) -> str:
        parsed = urllib.parse.urlsplit(self.base_url)
        scheme = "wss" if parsed.scheme == "https" else "ws"
        host = parsed.hostname or "localhost"
        if parsed.scheme == "https":
            port = parsed.port or 443
        else:
            try:
                port = (parsed.port or 8000) + self._DEFAULT_WS_PORT_SHIFT
            except ValueError:
                port = 8001
        return urllib.parse.urlunsplit((scheme, f"{host}:{port}", parsed.path, "", ""))

    def _build_ssl_ctx(self) -> Optional[ssl.SSLContext]:
        if self._verify is False:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return ctx
        if isinstance(self._verify, str):
            return ssl.create_default_context(cafile=self._verify)
        return None

    def _build_url(self, t_path: str, t_params: Optional[Dict[str, Any]]) -> str:
        query = ""
        if t_params:
            qs: List[str] = []
            for key, value in t_params.items():
                if isinstance(value, (list, tuple)):
                    for v in value:
                        qs.append(f"{key}={urllib.parse.quote(str(v))}")
                elif value is not None:
                    qs.append(f"{key}={urllib.parse.quote(str(value))}")
            if qs:
                query = "?" + "&".join(qs)
        return f"{self.base_url}{t_path}{query}"

    def _execute(
        self,
        t_method: str,
        t_path: str,
        *,
        t_params: Optional[Dict[str, Any]] = None,
        t_json_body: Any = None,
        t_raw_body: Optional[bytes] = None,
        t_content_type: Optional[str] = None,
        t_headers: Optional[Dict[str, str]] = None,
        t_as_json: bool = True,
    ) -> Any:
        url = self._build_url(t_path, t_params)

        hdrs = dict(self.headers)
        if t_headers:
            hdrs.update(t_headers)

        body: Optional[bytes] = None
        if t_json_body is not None:
            body = json.dumps(t_json_body).encode("utf-8")
            hdrs.setdefault("Content-Type", "application/json")
            hdrs.setdefault("Accept", "application/json")
        elif t_raw_body is not None:
            body = t_raw_body
            if t_content_type:
                hdrs.setdefault("Content-Type", t_content_type)

        req = urllib.request.Request(url, data=body, method=t_method, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=self._build_ssl_ctx()) as resp:
                payload = resp.read()
                if payload and t_as_json:
                    return json.loads(payload.decode("utf-8"))
                return payload
        except urllib.error.HTTPError as e:
            err_body: Any = None
            try:
                err_body = json.loads(e.read().decode("utf-8"))
            except Exception:
                pass
            raise GatewayHTTPError(e.code, url, err_body) from e
        except urllib.error.URLError as e:
            raise GatewayError(f"Network error reaching {url}: {e.reason}") from e

    # ── Registry endpoints ─────────────────────────────────────────────────────

    def registry(
        self,
        *,
        t_headers_only: bool = False,
        t_db: Optional[int] = None,
        t_cache: bool = False,
    ) -> RegistryResponse:
        """
        GET /registry — schema directory (all DBs, UDTs, tags).

        :param headers_only: return only DB headers (omit field trees).
        :param db: restrict the response to a single DB by number.
        :param cache: reuse (and populate) a full-registry cache; used by
            :meth:`read_db_array` so repeated calls don't re-fetch the
            schema on every read. Ignored when ``headers_only`` or ``db``
            narrow the response, since those aren't full snapshots.
        """
        if t_cache and not t_headers_only and t_db is None and self._registry_cache is not None:
            return self._registry_cache
        params: Dict[str, Any] = {}
        if t_headers_only:
            params["headers"] = "true"
        if t_db is not None:
            params["db"] = t_db
        data = self._execute("GET", "/registry", t_params=params)
        if not isinstance(data, dict):
            raise GatewayError("GET /registry returned malformed JSON")
        reg = RegistryResponse.from_dict(data)
        if t_cache and not t_headers_only and t_db is None:
            self._registry_cache = reg
        return reg

    def registry_types(self) -> Any:
        """GET /registry/types — the S7 type dictionary."""
        return self._execute("GET", "/registry/types")

    def modbus_map(self) -> Any:
        """
        GET /registry/modbus — the Modbus virtual register map.

        Raises :class:`GatewayHTTPError` (404) when Modbus is unsupported.
        """
        return self._execute("GET", "/registry/modbus")

    # ── Data endpoints (semantic twin) ─────────────────────────────────────────

    def read_data(self, t_path: Optional[str] = None) -> Any:
        """
        GET /data[/<path>] — full digital twin, one DB, or a subtree/leaf.

        :param path: e.g. ``"ReactorCore"``, ``"ReactorCore/speed"``,
            ``"DB2/temperatures/2"``. ``None`` reads the full twin.
        """
        url = "/data" if t_path in (None, "", "/") else f"/data/{t_path.lstrip('/')}"
        return self._execute("GET", url)

    read_db = read_data  # alias: ``gw.read_db("DB10")``

    def replace_field(self, t_path: str, t_value: Any) -> DataWriteResult:
        """PUT /data/<path> — full replacement of a field with a JSON value."""
        url = f"/data/{t_path.lstrip('/')}"
        data = self._execute("PUT", url, t_json_body=t_value)
        return DataWriteResult.from_dict(data or {})

    def write_fields(self, t_path: str, t_fields: Dict[str, Any]) -> DataWriteResult:
        """
        POST /data/<path> — atomic merge write of the given field object.

        :param path: DB name or dotted/slash field path (subtree root).
        :param fields: mapping of field name → value (partial merge; missing
            fields are unchanged).
        """
        url = f"/data/{t_path.lstrip('/')}"
        data = self._execute("POST", url, t_json_body=t_fields)
        return DataWriteResult.from_dict(data or {})

    def write_field(self, t_path: str, t_value: Any) -> DataWriteResult:
        """POST /data/<path> — scalar/leaf write at the resolved field path."""
        url = f"/data/{t_path.lstrip('/')}"
        data = self._execute("POST", url, t_json_body=t_value)
        return DataWriteResult.from_dict(data or {})

    def write_array_element(self, t_path: str, t_index: int, t_value: Any) -> DataWriteResult:
        """POST /data/<path>/<N> — scalar write to a single array element."""
        url = f"/data/{t_path.lstrip('/')}/{t_index}"
        data = self._execute("POST", url, t_json_body=t_value)
        return DataWriteResult.from_dict(data or {})

    def write_multi_db(self, t_payload: Dict[str, Dict[str, Any]]) -> Dict[str, Any]:
        """
        POST /data/ — atomic merge-write across multiple DBs.

        :param payload: ``{"DbName": {field: value, ...}, ...}``.
        """
        data = self._execute("POST", "/data", t_json_body=t_payload)
        return data if isinstance(data, dict) else {}

    # ── Raw memory endpoints (byte level) ──────────────────────────────────────

    def memory_read(
        self,
        t_db: int,
        t_offset: int,
        t_size: int,
        *,
        t_dtype: Any = None,
    ) -> Union[bytes, np.ndarray]:
        """
        GET /memory/db/<db>/offset/<o>/size/<s> — raw bytes (octet-stream).

        :param dtype: when given, the response is returned as a NumPy array
            of this dtype instead of raw ``bytes`` (e.g. ``">f4"`` for a
            single REAL, or a structured dtype from ``DbSchema.to_dtype``).
        """
        raw = self._execute("GET", f"/memory/db/{t_db}/offset/{t_offset}/size/{t_size}", t_as_json=False)
        if t_dtype is not None:
            return np.frombuffer(raw, dtype=t_dtype).copy()
        return raw

    def memory_write(self, t_db: int, t_offset: int, t_data: BytesLike) -> bytes:
        """
        PUT /memory/db/<db>/offset/<o>/size/<s> — raw byte write.

        :param data: raw ``bytes`` or a NumPy array/scalar (normalized via
            its buffer — build it with an S7 big-endian dtype from
            :mod:`sgrn.dtypes` so the bytes land correctly on the wire).

        The response echoes the written bytes (S7 confirmation semantics).
        """
        raw = as_bytes(t_data)
        return self._execute(
            "PUT",
            f"/memory/db/{t_db}/offset/{t_offset}/size/{len(raw)}",
            t_raw_body=raw,
            t_content_type="application/octet-stream",
            t_as_json=False,
        )

    def memory_batch_read(
        self,
        t_spans: List[MemorySpan],
        *,
        t_dtype: Any = None,
    ) -> Union[List[MemoryReadItem], List[np.ndarray]]:
        """
        GET /memory/batch — read from multiple DBs atomically.

        The query uses repeated ``db``/``offset``/``size`` triplets.

        :param dtype: when given, each item's bytes are returned as a
            NumPy array of this dtype instead of a :class:`MemoryReadItem`
            (use ``item.as_array(dtype)`` per-item if spans need different
            dtypes).
        """
        params: Dict[str, Any] = {}
        for span in t_spans:
            params.setdefault("db", []).append(span.db)
            params.setdefault("offset", []).append(span.offset)
            params.setdefault("size", []).append(span.size)
        data = self._execute("GET", "/memory/batch", t_params=params)
        items = [MemoryReadItem.from_dict(i) for i in (data or []) if isinstance(i, dict)]
        if t_dtype is not None:
            return [i.as_array(t_dtype) for i in items]
        return items

    def memory_batch_write(self, t_items: List[MemoryBatchWriteItem]) -> List[MemoryBatchWriteResult]:
        """
        PUT /memory/batch — atomic all-or-nothing multi-DB write (base64url).

        Each item's ``data`` may be raw ``bytes`` or a NumPy array/scalar.
        """
        payload = []
        for i in t_items:
            raw = as_bytes(i.data)
            payload.append({"db": i.db, "offset": i.offset, "size": len(raw), "data": to_base64url(raw)})
        data = self._execute("PUT", "/memory/batch", t_json_body=payload)
        items_out = data or []
        return [MemoryBatchWriteResult.from_dict(r) for r in items_out if isinstance(r, dict)]

    # ── Schema-driven NumPy access ───────────────────────────────────────────────

    def read_db_array(
        self,
        t_db: Union[str, int],
        *,
        t_registry: Optional[RegistryResponse] = None,
        t_cache_registry: bool = True,
    ) -> np.void:
        """
        Read one DB's full memory and return it as a single structured
        NumPy record, built from its registry schema — the fast path for
        ML workflows that want typed array access without hand-parsing
        ``read_data``'s JSON tree.

        :param db: DB name (``str``) or DB number (``int``).
        :param registry: reuse an already-fetched :class:`RegistryResponse`
            instead of issuing ``GET /registry`` again.
        :param cache_registry: when ``registry`` isn't given, cache the
            fetched registry on this client for subsequent calls.

        Example
        -------
        >>> rec = gw.read_db_array("ReactorCore")
        >>> float(rec["thermal_power_mw"])
        42.5
        """
        reg = t_registry or self.registry(cache=t_cache_registry)
        schema = reg.db_by_name(t_db) if isinstance(t_db, str) else reg.db_by_number(t_db)
        if schema is None:
            raise GatewayError(f"unknown DB {t_db!r} (not present in the registry)")
        dt = schema.to_dtype(t_udts=reg.udts_by_name())
        raw = self.memory_read(schema.db_number, 0, schema.size_bytes)
        return np.frombuffer(raw, dtype=dt, count=1)[0]

    # ── Diagnostics ─────────────────────────────────────────────────────────────

    def connections(self) -> List[ConnectionInfo]:
        """GET /connections — active/recent north- and south-bound connections."""
        data = self._execute("GET", "/connections")
        return [ConnectionInfo.from_dict(c) for c in (data or []) if isinstance(c, dict)]

    def db_history(self) -> Any:
        """GET /db/history — full historical database as JSON."""
        return self._execute("GET", "/db/history")

    def db_sessions(self) -> List[SessionInfo]:
        """GET /db/sessions — active and recent client sessions."""
        data = self._execute("GET", "/db/sessions")
        return [SessionInfo.from_dict(s) for s in (data or []) if isinstance(s, dict)]

    def db_logs(self, t_limit: int = 100) -> List[LogEntry]:
        """GET /db/logs?limit=<N> — most recent system log lines."""
        data = self._execute("GET", "/db/logs", t_params={"limit": t_limit})
        return [LogEntry.from_dict(l) for l in (data or []) if isinstance(l, dict)]

    def endpoints(self) -> List[EndpointInfo]:
        """GET /endpoints — API documentation as a list of endpoint entries."""
        data = self._execute("GET", "/endpoints")
        docs = data.get("endpoints", []) if isinstance(data, dict) else []
        return [EndpointInfo.from_dict(e) for e in docs if isinstance(e, dict)]

    def policy(self) -> SecurityPolicyResponse:
        """GET /api/policy — the live security policy."""
        data = self._execute("GET", "/api/policy")
        return SecurityPolicyResponse.from_dict(data or {})


# Backwards/collegial alias: the placeholder shipped as ``Gateway``; some
# integrations may prefer the longer name.
GatewayClient = Gateway
