#include <sgrn/ml/manifest.hpp>
#include <sgrn/utils/hashing.hpp>
#include <cmath>
#include <cstdio>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

} // namespace

int main() {
    using namespace sgrn::ml;

    ModelManifest manifest;
    manifest.name = "gas_turbine_anomaly";
    manifest.version = "2.1.0";
    manifest.backend = "torch";
    manifest.weights_file = "model.pt";

    // Set dummy weights checksum
    std::string mock_weights = "dummy_pytorch_model_weights_buffer_12345";
    auto hash_res = sgrn::utils::computeSha256Data(mock_weights, sgrn::utils::HashEncoding::hex);
    CHECK(!hash_res.hasError());
    manifest.weights_sha256 = hash_res.value();

    manifest.mlp.use_batch_norm = true;
    manifest.mlp.layers.push_back({64, "relu", 0.1f});
    manifest.mlp.layers.push_back({32, "relu", 0.0f});
    manifest.mlp.layers.push_back({1, "sigmoid", 0.0f});

    manifest.train.epochs_trained = 50;
    manifest.train.final_loss = 0.015f;
    manifest.train.metrics["accuracy"] = 0.985f;
    manifest.thresholds.warning = 0.6f;
    manifest.thresholds.alarm = 0.85f;
    manifest.thresholds.anomaly = 0.95f;

    // Check weights validation
    CHECK(!manifest.validateWeights(mock_weights).hasError());
    CHECK(manifest.validateWeights("corrupted_weights").hasError());

    // Roundtrip to JSON
    const std::string json = manifest.toJson();
    auto m2_res = ModelManifest::fromJson(json);
    CHECK(!m2_res.hasError());

    const auto& m2 = m2_res.value();
    CHECK(m2.name == "gas_turbine_anomaly");
    CHECK(m2.version == "2.1.0");
    CHECK(m2.backend == "torch");
    CHECK(m2.weights_file == "model.pt");
    CHECK(m2.weights_sha256 == manifest.weights_sha256);
    CHECK(m2.mlp.use_batch_norm == true);
    CHECK(m2.mlp.layers.size() == 3);
    CHECK(m2.mlp.layers[0].units == 64);
    CHECK(m2.mlp.layers[0].activation == "relu");
    CHECK(m2.mlp.layers[2].units == 1);
    CHECK(m2.train.epochs_trained == 50);
    CHECK(std::abs(m2.train.final_loss - 0.015f) < 1e-5f);
    CHECK(m2.train.metrics.at("accuracy") > 0.98f);
    CHECK(std::abs(m2.thresholds.warning - 0.6f) < 1e-5f);
    CHECK(std::abs(m2.thresholds.alarm - 0.85f) < 1e-5f);

    if (g_failures == 0) {
        std::printf("[manifest_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[manifest_test] %d tests FAILED.\n", g_failures);
    return 1;
}
