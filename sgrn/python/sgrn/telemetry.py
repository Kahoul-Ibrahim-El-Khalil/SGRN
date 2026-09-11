"""
telemetry.py — live digital-twin telemetry bindings for the SGRN Gateway.

Two layers, mirroring the client side of the embedded Svelte dashboard:

``GatewayTelemetry``
    A thin asyncio wrapper around the gateway's WebSocket stream, built on
    the ``websockets`` library (RFC 6455 handshake, framing, ping/pong, and
    reconnection are all handled there — nothing hand-rolled here). It
    speaks the wire protocol ``{"command": "subscribe"|"unsubscribe"|
    "clear_subscriptions", "path": <DbName or DbName/field>}``, seeds
    subscriptions on (re)connect, auto-reconnects, and dispatches parsed
    JSON frames and status changes to callbacks. It does NOT interpret the
    frames.

``TelemetryEngine``
    The abstracted "worker" pipeline used by the embedded web app
    (``web-svelte/src/lib/worker.ts``): incoming delta snapshots are
    flattened into a flat ``"<Db>-<path>"`` key map, filtered by the active
    subscription set, buffered, and flushed to a callback at a tunable rate.
    It is environment-agnostic — usable from notebooks, services, CLIs and
    other frameworks without a browser. Optionally, it can also accumulate
    each key's history into a fixed-size NumPy ring buffer
    (:class:`NumpyHistory`) for windowed feature extraction straight into
    an ML pipeline, without going through Python floats/lists first.

Wire format: the gateway pushes delta snapshots rooted at the Data Block
name, e.g. ``{"ReactorCore": {"thermal_power_mw": 100.5, "speed": 12.0}}``.
When a client subscribes to a leaf path, the server pre-prunes the payload,
so the same flattening logic applies to every frame.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any, Callable, Dict, Optional, Set

import numpy as np
import websockets
from websockets.exceptions import ConnectionClosed

__all__ = [
    "GatewayTelemetry",
    "TelemetryEngine",
    "TelemetryUpdate",
    "TelemetryBatch",
    "NumpyHistory",
]

log = logging.getLogger("sgrn.telemetry")

MessageCallback = Callable[[Dict[str, Any]], None]
BinaryMessageCallback = Callable[[int, float, np.ndarray], None]
StatusCallback = Callable[[str], None]
UpdateCallback = Callable[[Dict[str, "TelemetryUpdate"]], None]

# A single framed telemetry event.
TelemetryUpdate = Dict[str, Any]  # {"value": Any, "ts": float}
# Flat map: "<Db>-<path>" -> update, as flushed by TelemetryEngine.
TelemetryBatch = Dict[str, TelemetryUpdate]


class GatewayTelemetry:
    """
    Async WebSocket client for the gateway telemetry stream.

    Handles connection state, automatic reconnection, re-subscription after
    (re)connect, and dispatch of parsed JSON frames / status transitions.
    The RFC 6455 protocol itself (handshake, masking, fragmentation,
    ping/pong, close) is delegated entirely to the ``websockets`` package.
    """

    def __init__(
        self,
        t_url: str,
        *,
        t_reconnect_seconds: float = 5.0,
        t_open_timeout: float = 10.0,
        t_headers: Optional[Dict[str, str]] = None,
        t_origin: Optional[str] = None,
        t_on_message: Optional[MessageCallback] = None,
        t_on_binary: Optional[BinaryMessageCallback] = None,
        t_on_status: Optional[StatusCallback] = None,
        t_binary_dtypes: Optional[Dict[int, Any]] = None,
    ) -> None:
        self.url = t_url
        self.reconnect_seconds = max(0.5, t_reconnect_seconds)
        self.open_timeout = t_open_timeout
        self.headers = dict(t_headers or {})
        self.origin = t_origin
        self._on_message = t_on_message
        self._on_binary = t_on_binary
        self._on_status = t_on_status
        self.binary_dtypes = t_binary_dtypes or {}

        self.subscriptions: Set[str] = set()
        self.binary_subscriptions: Set[Tuple[int, int, Optional[int]]] = set()
        self._ws: Optional["websockets.asyncio.client.ClientConnection"] = None
        self._running = False
        self._task: Optional["asyncio.Task[None]"] = None
        self._status: str = "IDLE"

    # ── state ──────────────────────────────────────────────────────────────

    @property
    def connected(self) -> bool:
        return self._ws is not None and self._ws.state.name == "OPEN"

    @property
    def status(self) -> str:
        return self._status

    def subscribe_binary(self, t_db: int, t_offset: int = 0, t_size: Optional[int] = None) -> None:
        """
        Register a binary subscription for an entire Data Block (DB) or a contiguous slice.
        
        ARCHITECTURAL NOTE:
        Raw binary streaming operates *strictly on contiguous memory ranges*.
        You can subscribe to a full DB, or a slice (using `t_offset` and `t_size`), 
        but never scattered field-level paths.
        
        Why? By enforcing contiguous ranges, the C++ Gateway achieves true
        zero-copy streaming. The C++ WebSocket engine simply takes the arena pointer, 
        advances it (`arena.getPointer(db) + offset`), and dumps `size` bytes
        directly into the TCP socket send buffer. 
        
        If we allowed scattered field-level binary subscriptions, the C++ engine 
        would be forced to allocate intermediate buffers and `memcpy` scattered fields 
        together, defeating the entire purpose of raw memory access.
        """
        sub = (t_db, t_offset, t_size)
        self.binary_subscriptions.add(sub)
        if self.connected:
            self._send_command("subscribe_binary", db=t_db, offset=t_offset, size=t_size)

    def unsubscribe_binary(self, t_db: int, t_offset: int = 0, t_size: Optional[int] = None) -> None:
        sub = (t_db, t_offset, t_size)
        self.binary_subscriptions.discard(sub)
        if self.connected:
            self._send_command("unsubscribe_binary", db=t_db, offset=t_offset, size=t_size)

    def subscribe(self, t_path: str) -> None:
        """Register a local JSON subscription and push it to the gateway."""
        self.subscriptions.add(t_path)
        if self.connected:
            self._send_command("subscribe", path=t_path)

    def unsubscribe(self, t_path: str) -> None:
        self.subscriptions.discard(t_path)
        if self.connected:
            self._send_command("unsubscribe", path=t_path)

    def clear_subscriptions(self) -> None:
        self.subscriptions.clear()
        self.binary_subscriptions.clear()
        if self.connected:
            self._send_command("clear_subscriptions")

    def _send_command(
        self,
        t_command: str,
        *,
        path: Optional[str] = None,
        db: Optional[int] = None,
        offset: Optional[int] = None,
        size: Optional[int] = None,
    ) -> None:
        if self._ws is None:
            return
        payload: Dict[str, Any] = {"command": t_command}
        if path is not None:
            payload["path"] = path
        if db is not None:
            payload["db"] = db
        if offset is not None:
            payload["offset"] = offset
        if size is not None:
            payload["size"] = size
        # Never block a synchronous caller on the socket.
        asyncio.ensure_future(self._ws.send(json.dumps(payload)))

    # ── lifecycle ─────────────────────────────────────────────────────────────

    def start(self) -> "GatewayTelemetry":
        """Start the background receive/reconnect loop (requires a running loop)."""
        if self._task is not None:
            return self
        self._running = True
        self._task = asyncio.create_task(self._run())
        return self

    async def stop(self) -> None:
        self._running = False
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        if self._ws is not None:
            await self._ws.close()
            self._ws = None

    async def _run(self) -> None:
        while self._running:
            try:
                await self._connect_once()
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 — drive the reconnect policy
                log.debug("telemetry connection error: %s", e)
                self._report_status("DISCONNECTED")
            if not self._running:
                break
            await asyncio.sleep(self.reconnect_seconds)

    async def _connect_once(self) -> None:
        async with websockets.connect(
            self.url,
            additional_headers=self.headers or None,
            origin=self.origin,
            open_timeout=self.open_timeout,
        ) as ws:
            self._ws = ws
            self._report_status("CONNECTED")

            # Re-seed the server with our subscription set after every (re)connect.
            for path in sorted(self.subscriptions):
                await ws.send(json.dumps({"command": "subscribe", "path": path}))
            for db, offset, size in sorted(self.binary_subscriptions):
                payload = {"command": "subscribe_binary", "db": db}
                if offset is not None: payload["offset"] = offset
                if size is not None: payload["size"] = size
                await ws.send(json.dumps(payload))

            while self._running:
                try:
                    raw = await ws.recv()
                except ConnectionClosed:
                    log.debug("telemetry stream closed by peer")
                    break

                # -- Fast path for binary streams (Zero-Copy NumPy mapping) --
                if isinstance(raw, bytes):
                    if self._on_binary is not None and len(raw) >= 12:
                        import struct
                        # Header: [4 bytes DB Number (UInt32 Big Endian)] [8 bytes Timestamp (Float64 Big Endian)]
                        db_num, ts = struct.unpack(">Id", raw[:12])
                        dt = self.binary_dtypes.get(db_num)
                        if dt is not None:
                            try:
                                # Map the bytes immediately to NumPy using the pre-compiled dtype!
                                record = np.frombuffer(raw, dtype=dt, count=1, offset=12)[0]
                                self._on_binary(db_num, ts, record)
                            except Exception:
                                log.exception("on_binary callback failed")
                    continue

                # -- Standard path for JSON semantic twins --
                try:
                    data: Any = json.loads(raw)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    log.warning("non-JSON telemetry frame ignored")
                    continue
                if isinstance(data, dict) and self._on_message is not None:
                    try:
                        self._on_message(data)
                    except Exception:  # noqa: BLE001 — never kill the stream
                        log.exception("on_message callback failed")
        self._ws = None
        self._report_status("DISCONNECTED")

    def _report_status(self, t_status: str) -> None:
        self._status = t_status
        if self._on_status is not None:
            try:
                self._on_status(t_status)
            except Exception:  # noqa: BLE001
                log.exception("on_status callback failed")


def _is_subscribed(t_subscriptions: Set[str], t_db: str, t_path: str) -> bool:
    """Mirror ``worker.ts`` semantics for pruning updates by subscription."""
    if not t_subscriptions:
        return True  # firehose mode
    full_path = f"{t_db}/{t_path}" if t_path else t_db
    if full_path in t_subscriptions:
        return True
    for sub in t_subscriptions:
        if full_path.startswith(sub + "/") or sub == t_db:
            return True
    return False


class NumpyHistory:
    """
    Fixed-size NumPy ring buffer of (timestamp, value) per telemetry key.

    Feed it flushed batches from :class:`TelemetryEngine` (directly, or via
    ``TelemetryEngine(numpy_history=...)``) to keep a rolling float64 window
    per key ready for ML feature extraction — no Python list accumulation.
    Non-numeric values (bools/strings) are coerced with ``float()`` on a
    best-effort basis; values that can't be coerced are dropped.
    """

    def __init__(self, t_capacity: int = 512) -> None:
        if t_capacity < 1:
            raise ValueError("capacity must be >= 1")
        self.capacity = t_capacity
        self._values: Dict[str, np.ndarray] = {}
        self._timestamps: Dict[str, np.ndarray] = {}
        self._head: Dict[str, int] = {}
        self._count: Dict[str, int] = {}

    def _ensure(self, t_key: str) -> None:
        if t_key not in self._values:
            self._values[t_key] = np.full(self.capacity, np.nan, dtype=np.float64)
            self._timestamps[t_key] = np.zeros(self.capacity, dtype=np.float64)
            self._head[t_key] = 0
            self._count[t_key] = 0

    def ingest(self, t_batch: TelemetryBatch) -> None:
        """Absorb one flushed ``TelemetryEngine`` batch."""
        for key, update in t_batch.items():
            try:
                value = float(update.get("value"))
            except (TypeError, ValueError):
                continue
            self._ensure(key)
            h = self._head[key]
            self._values[key][h] = value
            self._timestamps[key][h] = float(update.get("ts", 0.0))
            self._head[key] = (h + 1) % self.capacity
            self._count[key] = min(self._count[key] + 1, self.capacity)

    def window(self, t_key: str, t_n: Optional[int] = None) -> np.ndarray:
        """Return the most recent ``n`` values for ``key``, oldest first."""
        if t_key not in self._values:
            return np.empty(0, dtype=np.float64)
        count = self._count[t_key]
        t_n = count if t_n is None else min(t_n, count)
        if t_n == 0:
            return np.empty(0, dtype=np.float64)
        head = self._head[t_key]
        idx = (head - t_n + np.arange(t_n)) % self.capacity
        return self._values[t_key][idx]

    def timestamps(self, t_key: str, t_n: Optional[int] = None) -> np.ndarray:
        if t_key not in self._timestamps:
            return np.empty(0, dtype=np.float64)
        count = self._count[t_key]
        t_n = count if t_n is None else min(t_n, count)
        if t_n == 0:
            return np.empty(0, dtype=np.float64)
        head = self._head[t_key]
        idx = (head - t_n + np.arange(t_n)) % self.capacity
        return self._timestamps[t_key][idx]

    def keys(self) -> Set[str]:
        return set(self._values.keys())


class TelemetryEngine:
    """
    Abstracted gateway telemetry pipeline (from the embedded Svelte worker).

    Feed it delta snapshots via :meth:`handle_frame` (as
    :class:`GatewayTelemetry` does automatically) and it will:

      * flatten nested delta snapshots into flat ``"<Db>-<path>"`` entries,
      * respect the active subscription set (firehose when empty),
      * buffer updates and emit them in a debounced batch (default 32 ms,
        ~30 Hz, exactly like the web worker),
      * optionally prune against an allow-list of known keys,
      * optionally mirror every flush into a :class:`NumpyHistory` ring
        buffer for windowed NumPy access to recent values.
    """

    def __init__(
        self,
        *,
        t_flush_interval_ms: float = 32.0,
        t_valid_keys: Optional[Set[str]] = None,
        t_on_batch: Optional[UpdateCallback] = None,
        t_numpy_history: Optional[NumpyHistory] = None,
    ) -> None:
        self.flush_interval_ms = max(8.0, min(float(t_flush_interval_ms), 1000.0))
        self.valid_keys: Optional[Set[str]] = set(t_valid_keys) if t_valid_keys else None
        self.on_batch = t_on_batch
        self.history = t_numpy_history

        self.subscriptions: Set[str] = set()
        self._buffer: "Dict[str, TelemetryUpdate]" = {}
        self._flush_timer: Optional[asyncio.Task] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    # ── subscriptions ───────────────────────────────────────────────────────

    def subscribe(self, t_path: str) -> None:
        self.subscriptions.add(t_path)

    def unsubscribe(self, t_path: str) -> None:
        self.subscriptions.discard(t_path)

    def clear_subscriptions(self) -> None:
        self.subscriptions.clear()

    def is_subscribed(self, t_db: str, t_path: str) -> bool:
        return _is_subscribed(self.subscriptions, t_db, t_path)

    # ── pipeline ────────────────────────────────────────────────────────────

    def handle_frame(self, t_data: Dict[str, Any]) -> None:
        """
        Ingest one delta snapshot (already parsed to a dict).

        Matches the ``onmessage`` processing of the embedded web worker:
        flatten into leaves, then schedule a debounced flush.
        """
        if not isinstance(t_data, dict):
            return
        self._loop = asyncio.get_running_loop() if self._loop is None else self._loop
        ts = time.time() * 1000.0
        for db, db_data in t_data.items():
            if isinstance(db_data, dict):
                self._flatten(db, "", db_data, ts)
            elif self.is_subscribed(db, ""):
                self._stage(db, "", db_data, ts)

        # Debounce: reset the flush timer on each frame (as the worker does).
        if self._flush_timer is not None:
            self._flush_timer.cancel()
        if self._buffer and self._loop is not None:
            delay = self.flush_interval_ms / 1000.0
            self._flush_timer = self._loop.create_task(self._delayed_flush(delay))

    def _flatten(self, t_db: str, t_path: str, t_value: Any, t_ts: float) -> None:
        if t_value is None:
            full_key = f"{t_db}-{t_path}"
            if not self.is_subscribed(t_db, t_path):
                return
            if t_path and self.valid_keys is not None and full_key not in self.valid_keys:
                return
            self._stage(t_db, t_path, None, t_ts)
            return

        if isinstance(t_value, list):
            for i, item in enumerate(t_value):
                sub_path = f"{t_path}/[{i}]" if t_path else f"[{i}]"
                self._flatten(t_db, sub_path, item, t_ts)
        elif isinstance(t_value, dict):
            for key, item in t_value.items():
                sub_path = f"{t_path}/{key}" if t_path else key
                self._flatten(t_db, sub_path, item, t_ts)
        else:
            full_key = f"{t_db}-{t_path}"
            if not self.is_subscribed(t_db, t_path):
                return
            if t_path and self.valid_keys is not None and full_key not in self.valid_keys:
                return
            self._stage(t_db, t_path, t_value, t_ts)

    def _stage(self, t_db: str, t_path: str, t_value: Any, t_ts: float) -> None:
        self._buffer[f"{t_db}-{t_path}"] = {"value": t_value, "ts": t_ts}

    async def _delayed_flush(self, t_delay: float) -> None:
        try:
            await asyncio.sleep(t_delay)
        except asyncio.CancelledError:
            return
        self.flush()

    def flush(self) -> None:
        """Immediately emit the buffered updates (mirrors worker ``flush``)."""
        if not self._buffer:
            return
        batch = dict(self._buffer)
        self._buffer.clear()
        if self.history is not None:
            self.history.ingest(batch)
        if self.on_batch is not None:
            try:
                self.on_batch(batch)
            except Exception:  # noqa: BLE001
                log.exception("on_batch callback failed")

    def reset(self) -> None:
        self._buffer.clear()
        if self._flush_timer is not None:
            self._flush_timer.cancel()
            self._flush_timer = None
