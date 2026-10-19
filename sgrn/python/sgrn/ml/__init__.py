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
    configureModelFromRegistry,
    configureModelFromSchema,
    inferDimensionFromName,
    inferPhysicalDimension,
    parseSclSchema,
    signalsFromRegistry,
    configure_model_from_registry,
    configure_model_from_schema,
    infer_dimension_from_name,
    infer_physical_dimension,
    parse_scl_schema,
    signals_from_registry,
)
from .transform import (
    Scaler,
    StreamingStats,
    contextEncodedDim,
    contextLayout,
    contextRowKeys,
    createSlidingWindows,
    encodeContextMatrix,
    extractContextMatrix,
    extractFeatureMatrix,
    extractRawContextColumns,
    filterProcessTimeline,
    fitProcessScaler,
    fitStratifiedStats,
    applyStratifiedStats,
    inferContextSpecs,
    splitProcessTimeline,
    streaming_stats,
    context_encoded_dim,
    context_layout,
    context_row_keys,
    create_sliding_windows,
    encode_context_matrix,
    extract_context_matrix,
    extract_feature_matrix,
    extract_raw_context_columns,
    filter_process_timeline,
    fit_process_scaler,
    fit_stratified_stats,
    apply_stratified_stats,
    infer_context_specs,
    split_process_timeline,
)
from .manifold import (
    ModePredictor,
    ProcessManifoldDetector,
    WindowAutoencoder,
    modeNLLs,
    trainManifoldDetector,
    trainModePredictor,
    mode_nlls,
    train_manifold_detector,
    train_mode_predictor,
)
from .pipeline import (
    evaluateManifoldSequence,
    fitManifoldPipeline,
    fitManifoldPipelineStream,
    inferStateAt,
    predictFeature,
    evaluate_manifold_sequence,
    fit_manifold_pipeline,
    fit_manifold_pipeline_stream,
    infer_state_at,
    predict_feature,
)
from .anomaly import (
    ConformalAnomalyDetector,
    SplitConformalCalibrator,
)

try:
    from .dataset import DatasetReader, BinaryDatasetReader, JsonlDatasetReader
except ImportError:
    DatasetReader = BinaryDatasetReader = JsonlDatasetReader = None

try:
    from .diagnostics import explainSample, fitMonitorAndDiagnoser, scoreDetections
except ImportError:
    explainSample = fitMonitorAndDiagnoser = scoreDetections = None

try:
    from .trainer import AutoMLTrainer
except ImportError:
    AutoMLTrainer = None

# ── Phase 1: PyTorch-native AutoML (replaces sklearn AutoMLTrainer) ──
try:
    from .pytorch_automl import PyTorchAutoMLTrainer, migrateAutoMLTrainer
except ImportError as _e:
    PyTorchAutoMLTrainer = migrateAutoMLTrainer = None

# ── Phase 2: XGBoost emulation via PyTorch (unified lifecycle) ──
try:
    from .xgboost_pytorch_emul import (
        PyXGBoostBooster,
        PyTorchXGBoostEmulator,
        getXGBoostCandidate,
    )
except ImportError:
    PyXGBoostBooster = PyTorchXGBoostEmulator = getXGBoostCandidate = None

# ── Phase 3: Manifold reconstruction-error features for AutoML ──
try:
    from .manifold_feature_integration import (
        ManifoldFeatureExtractor,
        augmentFeaturesWithManifold,
        getAutoMLAugmentedFeatureNames,
    )
except ImportError:
    ManifoldFeatureExtractor = None
    augmentFeaturesWithManifold = None
    getAutoMLAugmentedFeatureNames = None

# ── Phase 4: Unified anomaly pipeline (manifold+conformal+classical) ──
try:
    from .unified_anomaly import (
        UnifiedAnomalyDetector,
        fitUnifiedAnomalyDetector,
    )
except ImportError:
    UnifiedAnomalyDetector = None
    fitUnifiedAnomalyDetector = None

__all__ = [
    # Device / Hardware
    "getHardwareInfo",
    "resolveDevice",
    "get_hardware_info",
    "resolve_device",
    # Schema / Semantics
    "PHYSICAL_DIMENSION_MAP",
    "parseSclSchema",
    "signalsFromRegistry",
    "configureModelFromSchema",
    "configureModelFromRegistry",
    "inferPhysicalDimension",
    "inferDimensionFromName",
    "parse_scl_schema",
    "signals_from_registry",
    "configure_model_from_schema",
    "configure_model_from_registry",
    "infer_physical_dimension",
    "infer_dimension_from_name",
    # Data / Transforms
    "Scaler",
    "StreamingStats",
    "streaming_stats",
    "filterProcessTimeline",
    "extractFeatureMatrix",
    "splitProcessTimeline",
    "fitProcessScaler",
    "createSlidingWindows",
    "contextLayout",
    "contextEncodedDim",
    "contextRowKeys",
    "extractRawContextColumns",
    "inferContextSpecs",
    "fitStratifiedStats",
    "applyStratifiedStats",
    "encodeContextMatrix",
    "extractContextMatrix",
    "filter_process_timeline",
    "extract_feature_matrix",
    "split_process_timeline",
    "fit_process_scaler",
    "create_sliding_windows",
    "context_layout",
    "context_encoded_dim",
    "context_row_keys",
    "extract_raw_context_columns",
    "infer_context_specs",
    "fit_stratified_stats",
    "apply_stratified_stats",
    "encode_context_matrix",
    "extract_context_matrix",
    # Model / Manifold
    "WindowAutoencoder",
    "ModePredictor",
    "ProcessManifoldDetector",
    "trainManifoldDetector",
    "train_manifold_detector",
    "trainModePredictor",
    "train_mode_predictor",
    "modeNLLs",
    "mode_nlls",
    "ConformalAnomalyDetector",
    "SplitConformalCalibrator",
    # Pipelines
    "fitManifoldPipeline",
    "fit_manifold_pipeline",
    "fitManifoldPipelineStream",
    "fit_manifold_pipeline_stream",
    "inferStateAt",
    "infer_state_at",
    "predictFeature",
    "predict_feature",
    "evaluateManifoldSequence",
    "evaluate_manifold_sequence",
    # Diagnostics & Training
    "explainSample",
    "fitMonitorAndDiagnoser",
    "scoreDetections",
    "AutoMLTrainer",
    "DatasetReader",
    "BinaryDatasetReader",
    "JsonlDatasetReader",
    # PyTorch AutoML / XGBoost emulation / manifold features / unified anomaly
    "PyTorchAutoMLTrainer",
    "migrateAutoMLTrainer",
    "PyXGBoostBooster",
    "PyTorchXGBoostEmulator",
    "getXGBoostCandidate",
    "ManifoldFeatureExtractor",
    "augmentFeaturesWithManifold",
    "getAutoMLAugmentedFeatureNames",
    "UnifiedAnomalyDetector",
    "fitUnifiedAnomalyDetector",
]
