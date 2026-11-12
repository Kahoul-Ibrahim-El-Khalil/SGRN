#include "spec-json.hpp"
#include <sgrn/ml/manifest.hpp>
#include <sgrn/utils/glaze.hpp>
#include <sgrn/utils/hashing.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// JSON (Glaze mirror structs)
//
// The manifest JSON layout is a compatibility contract (manifests are written
// next to the trained model and read back on load). The shape is mirrored by a
// Json-suffixed struct whose member names match the JSON keys, so plain Glaze
// reflection reads and writes it (see spec-json.hpp for the shared FeatureSpec
// mirrors and the naming precedent).
//
// Member declaration order matches the historical key order byte-for-byte —
// that is why MlpConfigJson declares use_batch_norm before layers and
// TrainInfoJson puts dataset_sha256 before metrics, unlike the domain structs.
// Where a domain struct's member names, types and order already match the
// keys (MlpLayerConfig, ThresholdsConfig), it is reused directly. Absent keys
// keep the domain defaults on read.
// ─────────────────────────────────────────────────────────────────────────────

namespace sgrn::ml::detail
{

struct MlpConfigJson {
    bool use_batch_norm = false;
    std::vector<MlpLayerConfig> layers; ///< domain type reused: keys and order match
};

struct TrainInfoJson {
    uint64_t timestamp_ns = 0;
    uint32_t epochs_trained = 0;
    float final_loss = 0.0f;
    std::string dataset_sha256;
    std::map<std::string, float> metrics;
};

struct ModelManifestJson {
    std::string name;
    std::string version = "1.0.0";
    std::string backend = "torch";
    std::string weights_file;
    std::string weights_sha256;
    FeatureSpecJson spec;
    MlpConfigJson mlp;
    TrainInfoJson train;
    ThresholdsConfig thresholds; ///< domain type reused: keys and order match
};

} // namespace sgrn::ml::detail

namespace sgrn::ml
{

std::string ModelManifest::toJson(bool t_pretty) const {
    detail::ModelManifestJson w;
    w.name = name;
    w.version = version;
    w.backend = backend;
    w.weights_file = weights_file;
    w.weights_sha256 = weights_sha256;
    w.spec = detail::specToJson(spec);
    w.mlp.use_batch_norm = mlp.use_batch_norm;
    w.mlp.layers = mlp.layers;
    w.train.timestamp_ns = train.timestamp_ns;
    w.train.epochs_trained = train.epochs_trained;
    w.train.final_loss = train.final_loss;
    w.train.dataset_sha256 = train.dataset_sha256;
    w.train.metrics = train.metrics;
    w.thresholds = thresholds;

    const auto res = t_pretty ? sgrn::json::writePretty(w) : sgrn::json::write(w);
    // Plain mirror structs cannot fail to write; degrade to an empty payload.
    return res.value_or(std::string{});
}

Result<ModelManifest> ModelManifest::fromJson(std::string_view t_json_str) {
    auto w_res = sgrn::json::read<detail::ModelManifestJson>(t_json_str);
    RETURN_ERROR_IF(w_res.hasError(), "Failed to parse ModelManifest JSON: " + w_res.error().what());

    const auto& w = w_res.value();
    ModelManifest manifest;
    manifest.name = w.name;
    manifest.version = w.version;
    manifest.backend = w.backend;
    manifest.weights_file = w.weights_file;
    manifest.weights_sha256 = w.weights_sha256;
    manifest.spec = detail::specFromJson(w.spec);
    manifest.mlp.use_batch_norm = w.mlp.use_batch_norm;
    manifest.mlp.layers = std::move(w.mlp.layers);
    manifest.train.timestamp_ns = w.train.timestamp_ns;
    manifest.train.epochs_trained = w.train.epochs_trained;
    manifest.train.final_loss = w.train.final_loss;
    manifest.train.dataset_sha256 = std::move(w.train.dataset_sha256);
    manifest.train.metrics = std::move(w.train.metrics);
    manifest.thresholds = w.thresholds;
    return manifest;
}

Result<ModelManifest> ModelManifest::fromFile(const std::filesystem::path& t_file_path) {
    auto content = sgrn::json::readFileText(t_file_path);
    RETURN_ERROR_IF(content.hasError(), "Failed to open file: " + t_file_path.string());
    return fromJson(content.value());
}

Result<void> ModelManifest::validateWeights(std::string_view t_bytes) const {
    if (weights_sha256.empty()) {
        return {}; // No checksum specified
    }
    auto hash_res = sgrn::utils::computeSha256Data(t_bytes, sgrn::utils::HashEncoding::hex);
    RETURN_ERROR_IF(hash_res.hasError(), "Failed to compute SHA256 of weights: " + hash_res.error());
    RETURN_ERROR_IF(
        hash_res.value() != weights_sha256, "Weights SHA256 mismatch: expected " + weights_sha256 + ", got " + hash_res.value());
    return {};
}

Result<void> ModelManifest::validateWeightsFile(const std::filesystem::path& t_file_path) const {
    if (weights_sha256.empty()) {
        return {};
    }
    auto hash_res = sgrn::utils::computeSha256File(t_file_path, sgrn::utils::HashEncoding::hex);
    RETURN_ERROR_IF(hash_res.hasError(), "Failed to compute SHA256 of file " + t_file_path.string() + ": " + hash_res.error());
    RETURN_ERROR_IF(hash_res.value() != weights_sha256,
        "Weights file SHA256 mismatch for " + t_file_path.string() + ": expected " + weights_sha256 + ", got " + hash_res.value());
    return {};
}

} // namespace sgrn::ml
