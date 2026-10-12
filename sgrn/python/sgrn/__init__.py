"""
sgrn — Python bindings & Trustworthy AI toolkit for SGRN Digital Twin.

Public surface:

    Gateway / GatewayClient      typed HTTP REST client (all current endpoints)
    GatewayTelemetry             asyncio WebSocket telemetry client
    SemanticSchema / FieldMetadata  SCL schema semantic metadata layer
    ConformalAnomalyDetector     PyTorch Autoencoder + Split-Conformal Anomaly Detector
    LLMDoer / MockDoer           Grounded AI Doer Agent for fault recovery
    SchemaChecker                Tiered T1/T2 Safety Verification Layer
    ActionCompiler               Action compilation to s7shell AngelScript / WebSocket payloads
"""

try:
    from . import dtypes
except ImportError:
    dtypes = None

try:
    from .telemetry import GatewayTelemetry, NumpyHistory, TelemetryBatch, TelemetryEngine, TelemetryUpdate
except ImportError:
    GatewayTelemetry = None
    NumpyHistory = None
    TelemetryBatch = None
    TelemetryEngine = None
    TelemetryUpdate = None

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
    asBytes,
    toBase64url,
)
from .semantic import SemanticSchema, FieldMetadata
from .ml.anomaly import ConformalAnomalyDetector, SplitConformalCalibrator
from .ai import LLMDoer, MockDoer, PlantDoer, SchemaChecker, ValidationResult, ActionCompiler

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
    "NumpyHistory",
    "TelemetryBatch",
    "TelemetryUpdate",
    # Semantic Interoperability
    "SemanticSchema",
    "FieldMetadata",
    # ML Anomaly & Drift Detection
    "ConformalAnomalyDetector",
    "SplitConformalCalibrator",
    # Agentic AI & Trustworthy Verification
    "LLMDoer",
    "MockDoer",
    "PlantDoer",
    "SchemaChecker",
    "ValidationResult",
    "ActionCompiler",
    # Submodules
    "dtypes",
]
