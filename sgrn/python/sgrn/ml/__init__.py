"""
sgrn.ml — Machine Learning, Schema Integration & Process Anomaly Detection.

Organized across distinct functional domains:
  - sgrn.ml.device: Hardware discovery & GPU acceleration (NVIDIA CUDA, AMD ROCm, Apple MPS, CPU)
  - sgrn.ml.schema: SCL schema parsing, semantic dimension taxonomy & model auto-configuration
  - sgrn.ml.transform: Telemetry filtering, scaling & temporal sliding window generation
  - sgrn.ml.manifold: PyTorch autoencoder, manifold projection & conformal anomaly detection
  - sgrn.ml.pipeline: Composed end-to-end training and sequence evaluation workflows
  - sgrn.ml.dataset: WAL archive & binary persistence readers
  - sgrn.ml.diagnostics: Process monitoring & sample explanation helpers
"""

from .device import (
    getHardwareInfo,
    resolveDevice,
    get_hardware_info,
    resolve_device,
)
from .schema import (
    PHYSICAL_DIMENSION_MAP,
    configureModelFromSchema,
    inferDimensionFromName,
    inferPhysicalDimension,
    parseSclSchema,
    configure_model_from_schema,
    infer_dimension_from_name,
    infer_physical_dimension,
    parse_scl_schema,
)
from .transform import (
    Scaler,
    createSlidingWindows,
    extractFeatureMatrix,
    filterProcessTimeline,
    fitProcessScaler,
    splitProcessTimeline,
    create_sliding_windows,
    extract_feature_matrix,
    filter_process_timeline,
    fit_process_scaler,
    split_process_timeline,
)
from .manifold import (
    ProcessManifoldDetector,
    WindowAutoencoder,
    trainManifoldDetector,
    train_manifold_detector,
)
from .pipeline import (
    evaluateManifoldSequence,
    fitManifoldPipeline,
    evaluate_manifold_sequence,
    fit_manifold_pipeline,
)
from .anomaly import (
    ConformalAnomalyDetector,
    SplitConformalCalibrator,
)

try:
    from .dataset import DatasetReader, BinaryDatasetReader
    from .diagnostics import explainSample, fitMonitorAndDiagnoser
    from .trainer import AutoMLTrainer, trainModel
except ImportError:
    pass

__all__ = [
    # Device / Hardware
    "getHardwareInfo",
    "resolveDevice",
    "get_hardware_info",
    "resolve_device",
    # Schema / Semantics
    "PHYSICAL_DIMENSION_MAP",
    "parseSclSchema",
    "configureModelFromSchema",
    "inferPhysicalDimension",
    "inferDimensionFromName",
    "parse_scl_schema",
    "configure_model_from_schema",
    "infer_physical_dimension",
    "infer_dimension_from_name",
    # Data / Transforms
    "Scaler",
    "filterProcessTimeline",
    "extractFeatureMatrix",
    "splitProcessTimeline",
    "fitProcessScaler",
    "createSlidingWindows",
    "filter_process_timeline",
    "extract_feature_matrix",
    "split_process_timeline",
    "fit_process_scaler",
    "create_sliding_windows",
    # Model / Manifold
    "WindowAutoencoder",
    "ProcessManifoldDetector",
    "trainManifoldDetector",
    "train_manifold_detector",
    "ConformalAnomalyDetector",
    "SplitConformalCalibrator",
    # Pipelines
    "fitManifoldPipeline",
    "evaluateManifoldSequence",
    "fit_manifold_pipeline",
    "evaluate_manifold_sequence",
    # Diagnostics & Training
    "explainSample",
    "fitMonitorAndDiagnoser",
    "AutoMLTrainer",
    "trainModel",
    "DatasetReader",
    "BinaryDatasetReader",
]
