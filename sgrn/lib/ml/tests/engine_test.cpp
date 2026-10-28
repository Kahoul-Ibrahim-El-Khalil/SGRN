#include <sgrn/ml/engine.hpp>
#include <sgrn/ml/prediction_log.hpp>
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

// Mock Backend: outputs sum of features divided by input_size
class MockBackend : public sgrn::ml::IModelBackend {
public:
    sgrn::Result<void> load(const std::string&, const sgrn::ml::ModelManifest&) override {
        return {};
    }

    sgrn::Result<std::vector<float>> predict(std::span<const float> features) override {
        float sum = 0.0f;
        for (float v : features) {
            sum += v;
        }
        float out = features.empty() ? 0.0f : (sum / static_cast<float>(features.size()));
        return std::vector<float>{out};
    }

    size_t input_size() const override {
        return 2;
    }
    size_t output_size() const override {
        return 1;
    }
    const char* backend_name() const noexcept override {
        return "mock";
    }
};

} // namespace

int main() {
    using namespace sgrn::ml;

    // 1. BackendFactory registration & creation
    BackendFactory::registerBackend("mock", []() { return std::make_unique<MockBackend>(); });

    CHECK(BackendFactory::hasBackend("mock"));
    CHECK(!BackendFactory::hasBackend("nonexistent"));

    auto b_res = BackendFactory::createBackend("mock");
    CHECK(!b_res.hasError());
    auto backend = std::move(b_res.value());
    CHECK(backend->backend_name() == std::string("mock"));

    // 2. Setup manifest with 2 inputs and thresholds
    ModelManifest manifest;
    manifest.name = "unit_test_engine_model";
    manifest.backend = "mock";

    FeatureDef f1;
    f1.name = "temp";
    f1.tag_index = 0;
    f1.derived = DerivedType::Raw;
    manifest.spec.features.push_back(f1);

    FeatureDef f2;
    f2.name = "pressure";
    f2.tag_index = 1;
    f2.derived = DerivedType::Raw;
    manifest.spec.features.push_back(f2);

    manifest.thresholds.warning = 0.5f;
    manifest.thresholds.alarm = 0.75f;
    manifest.thresholds.anomaly = 0.90f;

    PredictionEngine engine;
    auto load_res = engine.loadModel(manifest, std::move(backend));
    CHECK(!load_res.hasError());
    CHECK(engine.hasModel());

    // 3. Test steps across threshold boundaries
    // Test Case A: Normal (avg = 0.2)
    {
        double inputs[] = {0.1, 0.3};
        auto pred_res = engine.step(1'000'000'000ULL, inputs);
        CHECK(!pred_res.hasError());
        const auto& p = pred_res.value();
        CHECK(p.verdict.status == VerdictStatus::Normal);
        CHECK(std::abs(p.verdict.anomaly_score - 0.2f) < 1e-4f);
    }

    // Test Case B: Warning (avg = 0.6)
    {
        double inputs[] = {0.6, 0.6};
        auto pred_res = engine.step(2'000'000'000ULL, inputs);
        CHECK(!pred_res.hasError());
        const auto& p = pred_res.value();
        CHECK(p.verdict.status == VerdictStatus::Warning);
        CHECK(std::abs(p.verdict.anomaly_score - 0.6f) < 1e-4f);
    }

    // Test Case C: Alarm (avg = 0.8)
    {
        double inputs[] = {0.7, 0.9};
        auto pred_res = engine.step(3'000'000'000ULL, inputs);
        CHECK(!pred_res.hasError());
        const auto& p = pred_res.value();
        CHECK(p.verdict.status == VerdictStatus::Alarm);
        CHECK(std::abs(p.verdict.anomaly_score - 0.8f) < 1e-4f);
    }

    // Test Case D: Anomaly (avg = 0.95)
    Prediction final_pred;
    {
        double inputs[] = {0.9, 1.0};
        auto pred_res = engine.step(4'000'000'000ULL, inputs);
        CHECK(!pred_res.hasError());
        final_pred = pred_res.value();
        CHECK(final_pred.verdict.status == VerdictStatus::Anomaly);
        CHECK(std::abs(final_pred.verdict.anomaly_score - 0.95f) < 1e-4f);
    }

    // 4. Test PredictionRecord binary serialization & round-trip
    {
        auto rec = PredictionRecord::fromPrediction(final_pred, 42);
        CHECK(rec.header.model_id == 42);
        CHECK(rec.header.verdict_status == static_cast<uint8_t>(VerdictStatus::Anomaly));
        CHECK(rec.header.num_outputs == 1);
        CHECK(std::abs(rec.header.anomaly_score - 0.95f) < 1e-4f);

        std::vector<uint8_t> buffer;
        rec.serialize(buffer);
        CHECK(buffer.size() == sizeof(PredictionRecordHeader) + sizeof(float));

        auto rec2_res = PredictionRecord::deserialize(buffer);
        CHECK(!rec2_res.hasError());
        const auto& rec2 = rec2_res.value();
        CHECK(rec2.header.timestamp_ns == 4'000'000'000ULL);
        CHECK(rec2.header.model_id == 42);
        CHECK(rec2.header.verdict_status == static_cast<uint8_t>(VerdictStatus::Anomaly));
        CHECK(rec2.outputs.size() == 1);
        CHECK(std::abs(rec2.outputs[0] - 0.95f) < 1e-4f);
    }

    if (g_failures == 0) {
        std::printf("[engine_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[engine_test] %d tests FAILED.\n", g_failures);
    return 1;
}
