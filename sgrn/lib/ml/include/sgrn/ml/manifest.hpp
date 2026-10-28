#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/feature_spec.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::ml
{

struct MlpLayerConfig {
    size_t units = 64;
    std::string activation = "relu"; // "relu", "gelu", "tanh", "sigmoid", "none"
    float dropout = 0.0f;
};

struct MlpConfig {
    std::vector<MlpLayerConfig> layers;
    bool use_batch_norm = false;
};

struct TrainInfo {
    uint64_t timestamp_ns = 0;
    uint32_t epochs_trained = 0;
    float final_loss = 0.0f;
    std::map<std::string, float> metrics;
    std::string dataset_sha256;
};

struct ThresholdsConfig {
    float warning = 0.5f;
    float alarm = 0.8f;
    float anomaly = 0.95f;
};

class ModelManifest {
public:
    ModelManifest() = default;

    std::string name;
    std::string version = "1.0.0";
    std::string backend = "torch"; // "torch", "linear", etc.
    std::string weights_file;
    std::string weights_sha256;

    FeatureSpec spec;
    MlpConfig mlp;
    TrainInfo train;
    ThresholdsConfig thresholds;

    [[nodiscard]] std::string toJson(bool t_pretty = false) const;
    static Result<ModelManifest> fromJson(std::string_view t_json_str);
    static Result<ModelManifest> fromFile(const std::filesystem::path& t_file_path);

    [[nodiscard]] Result<void> validateWeights(std::string_view t_bytes) const;
    [[nodiscard]] Result<void> validateWeightsFile(const std::filesystem::path& t_file_path) const;
};

} // namespace sgrn::ml
