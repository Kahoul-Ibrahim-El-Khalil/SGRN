"""
websocket.py — minimal, dependency-free RFC 6455 WebSocket client (asyncio).

Implements just enough of the protocol for the gateway telemetry stream:
handshake, masked client frames, unmasked server frames, fragment
reassembly, ping/pong, and close. No third-party dependencies are required,
which keeps the SGRN bindings usable on minimal/offline runtimes.

Exposed pieces:
    WebSocket            async client with ``connect/send_text/send_bytes/recv/close``
    WebSocketError       protocol / transport error
    WebSocketClosed      raised by :meth:`WebSocket.recv` after the peer closes
    encode_frame         frame builder (also used by echo servers / tests)
    FrameDecoder         incremental frame parser (also used by tests)
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import ssl
import struct
import urllib.parse
from collections import deque
from typing import Dict, List, Optional, Tuple

__all__ = [
    "WebSocket",
    "WebSocketError",
    "WebSocketClosed",
    "compute_accept_key",
    "encode_frame",
    "Frame",
]

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_MAX_FRAME_SIZE = 4 * 1024 * 1024  # 4 MiB guard rail

# Opcodes
_OP_CONT = 0x0
_OP_TEXT = 0x1
_OP_BIN = 0x2
_OP_CLOSE = 0x8
_OP_PING = 0x9
_OP_PONG = 0xA


class WebSocketError(Exception):
    """Transport or protocol error."""


class WebSocketClosed(WebSocketError):
    """Raised when the peer closes the connection (cleanly or not)."""


def compute_accept_key(t_key: str) -> str:
    """Sec-WebSocket-Accept value for the given Sec-WebSocket-Key."""
    digest = hashlib.sha1((t_key + _GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def encode_frame(t_payload: bytes, t_opcode: int, *, t_client_masked: bool = True) -> bytes:
    """Build a single (non-fragmented) WebSocket frame."""
    if len(t_payload) > _MAX_FRAME_SIZE:
        raise WebSocketError(f"payload too large: {len(t_payload)} bytes")

    first = 0x80 | t_opcode  # FIN bit set
    header = bytearray([first])

    length = len(t_payload)
    mask_bit = 0x80 if t_client_masked else 0x00
    if length < 126:
        header.append(mask_bit | length)
    elif length <= 0xFFFF:
        header.append(mask_bit | 126)
        header += struct.pack(">H", length)
    else:
        header.append(mask_bit | 127)
        header += struct.pack(">Q", length)

    if t_client_masked:
        mask = os.urandom(4)
        header += mask
        t_payload = bytes(b ^ mask[i % 4] for i, b in enumerate(t_payload))

    return bytes(header) + t_payload


class Frame:
    """A finalized (defragmented) message frame."""

    __slots__ = ("opcode", "payload")

    def __init__(self, t_opcode: int, t_payload: bytes):
        self.opcode = t_opcode
        self.payload = t_payload


class FrameDecoder:
    """
    Incremental parser for frames read from the wire.

    Handles client-side masking, fragmentation (continuation frames are
    aggregated), and detection of ping/pong/close control frames.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()
        self._frag_opcode: Optional[int] = None
        self._frag_parts: List[bytes] = []

    def feed(self, t_data: bytes) -> List[Frame]:
        """Feed raw bytes; return any fully parsed frames."""
        self._buffer += t_data
        frames: List[Frame] = []
        while True:
            frame = self._parse_one()
            if frame is None:
                break
            frames.append(frame)
        return frames

    def _parse_one(self) -> Optional[Frame]:
        buf = self._buffer
        if len(buf) < 2:
            return None
        b0, b1 = buf[0], buf[1]
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        length = b1 & 0x7F

        offset = 2
        if length == 126:
            if len(buf) < offset + 2:
                return None
            length = struct.unpack(">H", buf[offset : offset + 2])[0]
            offset += 2
        elif length == 127:
            if len(buf) < offset + 8:
                return None
            length = struct.unpack(">Q", buf[offset : offset + 8])[0]
            offset += 8

        if length > _MAX_FRAME_SIZE:
            raise WebSocketError("frame too large")

        mask = b""
        if masked:
            if len(buf) < offset + 4:
                return None
            mask = bytes(buf[offset : offset + 4])
            offset += 4

        if len(buf) < offset + length:
            return None

        payload = bytes(buf[offset : offset + length])
        del buf[: offset + length]

        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

        # Control frames must never be fragmented.
        if opcode in (_OP_CLOSE, _OP_PING, _OP_PONG):
            return Frame(opcode, payload)

        if opcode == _OP_CONT:
            if self._frag_opcode is None:
                raise WebSocketError("continuation frame without a start frame")
            self._frag_parts.append(payload)
            if fin:
                result = Frame(self._frag_opcode, b"".join(self._frag_parts))
                self._frag_opcode = None
                self._frag_parts = []
                return result
            return None

        if opcode in (_OP_TEXT, _OP_BIN):
            if fin:
                return Frame(opcode, payload)
            self._frag_opcode = opcode
            self._frag_parts = [payload]
            return None

        raise WebSocketError(f"unknown opcode 0x{opcode:x}")


class WebSocket:
    """An asyncio RFC 6455 client connection."""

    def __init__(
        self,
        t_url: str,
        *,
        t_timeout: float = 10.0,
        t_headers: Optional[Dict[str, str]] = None,
        t_origin: Optional[str] = None,
        t_ssl_context: Optional[ssl.SSLContext] = None,
    ) -> None:
        self.url = t_url
        self.timeout = t_timeout
        self.headers = dict(t_headers or {})
        if t_origin:
            self.headers.setdefault("Origin", t_origin)
        self._ssl_context = t_ssl_context
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._decoder = FrameDecoder()
        self._incoming: "deque[Frame]" = deque()
        self._closed = False

    # ── lifecycle ────────────────────────────────────────────────────────

    async def connect(self) -> None:
        parsed = urllib.parse.urlsplit(self.url)
        if parsed.scheme not in ("ws", "wss"):
            raise WebSocketError(f"unsupported URL scheme: {parsed.scheme!r}")
        host = parsed.hostname
        if not host:
            raise WebSocketError("missing host in URL")
        port = parsed.port or (443 if parsed.scheme == "wss" else 80)

        ctx = self._ssl_context
        if parsed.scheme == "wss" and ctx is None:
            ctx = ssl.create_default_context()

        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(host, port, ssl=ctx), timeout=self.timeout
        )

        key = base64.b64encode(os.urandom(16)).decode("ascii")
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        host_header = host
        if (parsed.scheme == "ws" and port != 80) or (parsed.scheme == "wss" and port != 443):
            host_header = f"{host}:{port}"

        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {host_header}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        for k, v in self.headers.items():
            lines.append(f"{k}: {v}")
        request = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8")

        assert self._writer is not None
        self._writer.write(request)
        await self._writer.drain()

        head = await self._read_until(b"\r\n\r\n")
        status, resp_headers = self._parse_handshake(head.decode("latin-1"))
        if status != 101:
            raise WebSocketError(f"handshake failed: HTTP {status}")
        if resp_headers.get("sec-websocket-accept", "").strip() != compute_accept_key(key):
            raise WebSocketError("handshake failed: bad Sec-WebSocket-Accept")

    async def close(self, t_code: int = 1000, t_reason: str = "") -> None:
        if self._closed:
            return
        self._closed = True
        try:
            if self._writer and not self._writer.is_closing():
                payload = struct.pack(">H", t_code) + t_reason.encode("utf-8")
                self._writer.write(encode_frame(payload, _OP_CLOSE, client_masked=True))
                await self._writer.drain()
        except Exception:
            pass
        if self._writer:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass

    @property
    def closed(self) -> bool:
        return self._closed

    # ── send ─────────────────────────────────────────────────────────────

    async def send_text(self, t_text: str) -> None:
        await self._send_frame(encode_frame(t_text.encode("utf-8"), _OP_TEXT, client_masked=True))

    async def send_bytes(self, t_payload: bytes) -> None:
        await self._send_frame(encode_frame(t_payload, _OP_BIN, client_masked=True))

    async def _send_frame(self, t_frame: bytes) -> None:
        if self._writer is None or self._closed:
            raise WebSocketError("connection is not open")
        self._writer.write(t_frame)
        await self._writer.drain()
# ── receive ──────────────────────────────────────────────────────────────

    async def recv(self) -> Frame:
        """
        Return the next text or binary frame.

        Ping frames are answered automatically; a close frame raises
        :class:`WebSocketClosed`.
        """
        while True:
            while self._incoming:
                frame = self._incoming.popleft()
                if frame.opcode == _OP_PING and self._writer and not self._writer.is_closing():
                    self._writer.write(encode_frame(frame.payload, _OP_PONG, client_masked=True))
                    await self._writer.drain()
                    continue
                if frame.opcode == _OP_CLOSE:
                    self._closed = True
                    raise WebSocketClosed("peer closed the connection")
                return frame
            await self._refill()

    async def _refill(self) -> None:
        """Block until at least one parsed frame is ready."""
        while not self._incoming:
            if not self._reader:
                raise WebSocketError("connection is not open")
            try:
                data = await asyncio.wait_for(self._reader.read(4096), timeout=self.timeout)
            except asyncio.TimeoutError as e:
                raise WebSocketError("receive timeout") from e
            if not data:
                self._closed = True
                raise WebSocketClosed("connection closed by peer")
            self._incoming.extend(self._decoder.feed(data))

    # ── handshake plumbing ────────────────────────────────────────────────

    async def _read_until(self, t_marker: bytes) -> bytes:
        if not self._reader:
            raise WebSocketError("connection is not open")
        chunks: List[bytes] = []
        while True:
            try:
                data = await asyncio.wait_for(self._reader.read(4096), timeout=self.timeout)
            except asyncio.TimeoutError as e:
                raise WebSocketError("handshake timeout") from e
            if not data:
                raise WebSocketError("server closed during handshake")
            chunks.append(data)
            joined = b"".join(chunks)
            if t_marker in joined:
                return joined[: joined.index(t_marker) + len(t_marker)]

    @staticmethod
    def _parse_handshake(t_head: str) -> Tuple[int, Dict[str, str]]:
        status = 0
        headers: Dict[str, str] = {}
        for idx, line in enumerate(t_head.split("\r\n")):
            if idx == 0:
                parts = line.split(" ", 2)
                if len(parts) >= 2 and parts[0].startswith("HTTP/"):
                    try:
                        status = int(parts[1])
                    except ValueError:
                        status = 0
                continue
            if ":" in line:
                key, _, value = line.partition(":")
                headers[key.strip().lower()] = value.strip()
        return status, headers


# Convenience constants (also re-exported for echo servers / tests)
OPCODE_TEXT = _OP_TEXT
OPCODE_BINARY = _OP_BIN
OPCODE_CLOSE = _OP_CLOSE