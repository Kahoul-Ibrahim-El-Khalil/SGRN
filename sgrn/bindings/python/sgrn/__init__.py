"""
sgrn — Python bindings for the SGRN Gateway.

Public surface:

    Gateway / GatewayClient      typed HTTP REST client (all current endpoints)
    GatewayTelemetry             asyncio WebSocket telemetry client (built on ``websockets``)
    TelemetryEngine              flatten/filter/buffer telemetry pipeline
    NumpyHistory                 rolling NumPy ring buffer for telemetry
    dtypes.*                     S7 <-> NumPy dtype bridge (registry schema -> structured dtype)
    models.*                     dataclasses for every gateway JSON shape

See ``sgrn/bindings/python/README.md`` for the full reference.
"""

from . import dtypes
from .gateway import Gateway, GatewayClient, GatewayError, GatewayHTTPError
from .models import (
    ConnectionInfo,
    DataWriteResult,
    DbField,
    DbSchema,
    EndpointInfo,
    LogEntry,
    MemoryBatchWriteItem,
    MemoryBatchWriteResult,
    MemoryReadItem,
    MemorySpan,
    RegistryResponse,
    SecurityPolicyResponse,
    SecurityRule,
    SessionInfo,
    SymbolTag,
    UdtSchema,
    as_bytes,
    to_base64url,
)
from .telemetry import GatewayTelemetry, NumpyHistory, TelemetryBatch, TelemetryEngine, TelemetryUpdate

__version__ = "0.2.0"

__all__ = [
    # HTTP client
    "Gateway",
    "GatewayClient",
    "GatewayError",
    "GatewayHTTPError",
    # Telemetry
    "GatewayTelemetry",
    "TelemetryEngine",
    "TelemetryUpdate",
    "TelemetryBatch",
    "NumpyHistory",
    # NumPy bridge
    "dtypes",
    # Models
    "DbField",
    "DbSchema",
    "UdtSchema",
    "SymbolTag",
    "RegistryResponse",
    "MemorySpan",
    "MemoryReadItem",
    "MemoryBatchWriteItem",
    "MemoryBatchWriteResult",
    "DataWriteResult",
    "EndpointInfo",
    "ConnectionInfo",
    "SessionInfo",
    "LogEntry",
    "SecurityRule",
    "SecurityPolicyResponse",
    "to_base64url",
    "as_bytes",
    "__version__",
]
