#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/ml/backend.hpp>
#include <sgrn/ml/engine.hpp>
#include <sgrn/ml/manifest.hpp>
#include <s7codec/codec.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sgrn::ml_bridge
{

// sgrn::ml types consumed by the bridge (declared here once, used unqualified
// below — this component sits one layer above sgrn::ml).
using sgrn::ml::IModelBackend;
using sgrn::ml::ModelManifest;
using sgrn::ml::Prediction;
using sgrn::ml::PredictionEngine;
using sgrn::ml::VerdictStatus;

using ::sgrn::gateway::twin::DbEntry;
using ::sgrn::gateway::twin::PlcNode;
using ::sgrn::gateway::twin::PlcState;

/**
 * @brief Output target type specifying which part of the prediction is written to a PlcNode.
 */
enum class TwinOutputTarget : uint8_t {
    PredictionValue, ///< Single predicted scalar from Prediction::outputs[index]
    AnomalyScore,    ///< Float anomaly score from Verdict
    VerdictStatus,   ///< Integer verdict status (Normal=0, Warning=1, Alarm=2, Anomaly=3)
    AlarmBit,        ///< Boolean flag: true if status is Alarm or Anomaly
    Timestamp        ///< 64-bit nanosecond timestamp from Prediction::timestamp_ns
};

/**
 * @brief Binding configuration mapping a model output to a digital twin tag path.
 */
struct TwinOutputBinding {
    std::string tag_path;
    TwinOutputTarget target{TwinOutputTarget::PredictionValue};
    size_t output_index{0}; ///< Index in Prediction::outputs (when target is PredictionValue)
};

/**
 * @brief Configuration parameters for the TwinBridge runtime.
 */
struct TwinBridgeConfig {
    std::vector<TwinOutputBinding> output_bindings;
    bool write_outputs_to_twin{true}; ///< Automatically commit outputs to PlcState upon step()
    bool bump_versions{true};         ///< Bump PlcNodeState version chain on output write
    bool mark_dirty{true};            ///< Mark affected ArenaTree::Segment dirty on write
};

/**
 * @brief Cached descriptor for a feature input bound to a specific PlcNode in physical memory.
 */
struct BoundFeatureNode {
    const PlcNode* p_node{nullptr};
    DbEntry* p_entry{nullptr};
    size_t feature_index{0};
    uint16_t db_number{0};
    size_t byte_offset{0};
    uint8_t bit_index{0};
    s7codec::Type type{s7codec::Type::Byte};
    s7codec::Endian endian{s7codec::Endian::Little};
    bool valid{false};
};

/**
 * @brief Cached descriptor for an output target bound to a specific PlcNode in physical memory.
 */
struct BoundOutputNode {
    const PlcNode* p_node{nullptr};
    DbEntry* p_entry{nullptr};
    TwinOutputTarget target{TwinOutputTarget::PredictionValue};
    size_t output_index{0};
    uint16_t db_number{0};
    size_t byte_offset{0};
    uint8_t bit_index{0};
    s7codec::Type type{s7codec::Type::Byte};
    s7codec::Endian endian{s7codec::Endian::Little};
    bool valid{false};
};

/**
 * @brief Bridge integrating the SGRN Digital Twin (PlcState) with the ML PredictionEngine.
 *
 * Provides zero-allocation, typed extraction of PLC variables into neural network inputs,
 * and handles closed-loop injection of model predictions/verdicts back into physical twin memory.
 */
class TwinBridge {
public:
    TwinBridge() = default;
    explicit TwinBridge(TwinBridgeConfig t_config);
    ~TwinBridge() = default;

    TwinBridge(const TwinBridge&) = delete;
    TwinBridge& operator=(const TwinBridge&) = delete;
    TwinBridge(TwinBridge&&) noexcept = default;
    TwinBridge& operator=(TwinBridge&&) noexcept = default;

    /// Loads model manifest and backend into internal PredictionEngine
    Result<void> loadModel(ModelManifest t_manifest, std::unique_ptr<IModelBackend> t_backend);

    /// Binds feature specifications and output mappings to a live PlcState
    Result<void> bindState(PlcState& t_state);

    /// Unbinds the state
    void unbindState() noexcept;

    /// Runs one inference tick using current values from the bound PlcState
    Result<Prediction> step(uint64_t t_ts_ns);

    /// Runs inference from an explicit DB frame snapshot buffer (e.g. historical archive frame)
    Result<Prediction> stepSnapshot(uint64_t t_ts_ns, uint16_t t_db_number, std::span<const uint8_t> t_raw_db);

    /// Resets internal feature extractor state (ring buffers, lags) and prediction engine
    void reset();

    [[nodiscard]] bool isBound() const noexcept {
        return p_state_ != nullptr;
    }

    [[nodiscard]] const PredictionEngine& engine() const noexcept {
        return engine_;
    }

    [[nodiscard]] PredictionEngine& engine() noexcept {
        return engine_;
    }

    [[nodiscard]] const ModelManifest* manifest() const noexcept {
        return engine_.manifest();
    }

    [[nodiscard]] const std::vector<BoundFeatureNode>& boundFeatures() const noexcept {
        return bound_inputs_;
    }

    [[nodiscard]] const std::vector<BoundOutputNode>& boundOutputs() const noexcept {
        return bound_outputs_;
    }

    [[nodiscard]] const TwinBridgeConfig& config() const noexcept {
        return config_;
    }

    void setConfig(TwinBridgeConfig t_config) {
        config_ = std::move(t_config);
    }

    /// Explicitly writes prediction results to the bound twin state
    Result<void> writeOutputsToTwin(const Prediction& t_pred);

private:
    double extractScalarValue(const uint8_t* tp_arena, const BoundFeatureNode& t_node) const;

    TwinBridgeConfig config_;
    PredictionEngine engine_;
    PlcState* p_state_{nullptr};
    std::vector<BoundFeatureNode> bound_inputs_;
    std::vector<BoundOutputNode> bound_outputs_;
    std::vector<double> raw_feature_scratch_;
};

} // namespace sgrn::ml_bridge
