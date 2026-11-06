#include <sgrn/ml/engine.hpp>
#include <sgrn/ml_torch/backend.hpp>
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
    using namespace sgrn::ml_torch;

    // 1. Factory registration
    TorchBackend::registerWithFactory();
    CHECK(BackendFactory::hasBackend("torch"));

    auto b_res = BackendFactory::createBackend("torch");
    CHECK(!b_res.hasError());
    auto backend = std::move(b_res.value());
    CHECK(backend->backend_name() == std::string("torch"));

    // 2. Setup manifest
    ModelManifest manifest;
    manifest.name = "torch_test_model";
    manifest.backend = "torch";

    FeatureDef f1;
    f1.name = "speed";
    f1.tag_index = 0;
    f1.derived = DerivedType::Raw;
    manifest.spec.features.push_back(f1);

    FeatureDef f2;
    f2.name = "torque";
    f2.tag_index = 1;
    f2.derived = DerivedType::Raw;
    manifest.spec.features.push_back(f2);

    LabelDef l0;
    l0.name = "anomaly_prob";
    manifest.spec.labels.push_back(l0);

    manifest.mlp.layers.push_back({8, "relu", 0.0f});
    manifest.mlp.layers.push_back({1, "sigmoid", 0.0f});

    manifest.thresholds.warning = 0.5f;
    manifest.thresholds.alarm = 0.8f;
    manifest.thresholds.anomaly = 0.95f;

    // 3. Load backend with manifest (in-memory weights initialization)
    auto load_res = backend->load("", manifest);
    CHECK(!load_res.hasError());
    CHECK(backend->input_size() == 2);
    CHECK(backend->output_size() == 1);

    // 4. Test inference via span
    float sample[] = {0.5f, -0.2f};
    auto pred_res = backend->predict(sample);
    CHECK(!pred_res.hasError());
    CHECK(pred_res.value().size() == 1);
    CHECK(pred_res.value()[0] >= 0.0f && pred_res.value()[0] <= 1.0f); // Sigmoid range

    // 5. Integrate into PredictionEngine
    PredictionEngine engine;
    auto eng_res = engine.loadModel(manifest, std::move(backend));
    CHECK(!eng_res.hasError());
    CHECK(engine.hasModel());

    double raw_inputs[] = {0.5, -0.2};
    auto step_res = engine.step(1'000'000'000ULL, raw_inputs);
    CHECK(!step_res.hasError());
    const auto& p = step_res.value();
    CHECK(p.outputs.size() == 1);
    CHECK(p.features.size() == 2);
    CHECK(p.latency_us > 0);

    if (g_failures == 0) {
        std::printf("[torch_backend_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[torch_backend_test] %d tests FAILED.\n", g_failures);
    return 1;
}
