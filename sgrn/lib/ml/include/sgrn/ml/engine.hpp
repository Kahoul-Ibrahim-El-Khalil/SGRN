#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/backend.hpp>
#include <sgrn/ml/extractor.hpp>
#include <sgrn/ml/manifest.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sgrn::ml
{

enum class VerdictStatus : uint8_t { Normal = 0, Warning = 1, Alarm = 2, Anomaly = 3 };

struct Verdict {
    VerdictStatus status = VerdictStatus::Normal;
    float anomaly_score = 0.0f;
    float confidence = 1.0f;
    std::string message;
};

struct Prediction {
    uint64_t timestamp_ns = 0;
    std::vector<float> outputs;
    std::vector<float> features;
    Verdict verdict;
    uint32_t latency_us = 0;
};

class PredictionEngine {
public:
    PredictionEngine() = default;

    Result<void> loadModel(ModelManifest t_manifest, std::unique_ptr<IModelBackend> t_backend);
    [[nodiscard]] bool hasModel() const noexcept {
        return backend_ != nullptr;
    }

    [[nodiscard]] const ModelManifest* manifest() const noexcept {
        return hasModel() ? &manifest_ : nullptr;
    }

    Result<Prediction> step(uint64_t t_ts_ns, std::span<const double> t_raw_inputs);
    void reset();

private:
    ModelManifest manifest_;
    std::unique_ptr<IModelBackend> backend_;
    FeatureExtractor extractor_;
    std::vector<float> feature_buffer_;
};

} // namespace sgrn::ml
