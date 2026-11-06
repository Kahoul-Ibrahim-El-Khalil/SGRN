#include <sgrn/ml_torch/backend.hpp>
#include <sgrn/ml_torch/trainer.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>

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

    // 1. Generate synthetic dataset: y = 2.0 * x0 - 3.0 * x1
    torch::manual_seed(42);
    const int64_t n_samples = 200;
    torch::Tensor x = torch::randn({n_samples, 2});
    torch::Tensor y = 2.0f * x.select(1, 0) - 3.0f * x.select(1, 1);
    y = y.unsqueeze(1);

    // 2. Build Model
    sgrn::ml::MlpConfig mlp_cfg;
    mlp_cfg.layers.push_back({32, "relu", 0.0f});

    Mlp model(2, mlp_cfg, 1);

    // 3. Configure Trainer
    TrainerConfig train_cfg;
    train_cfg.epochs = 20;
    train_cfg.batch_size = 16;
    train_cfg.learning_rate = 0.05;
    train_cfg.optimizer = "adamw";
    train_cfg.loss = "mse";
    train_cfg.validation_split = 0.2f;

    Trainer trainer;
    ModelManifest manifest;
    manifest.name = "linear_synthetic_model";
    manifest.backend = "torch";
    manifest.mlp = mlp_cfg;

    FeatureDef f0;
    f0.name = "x0";
    f0.tag_index = 0;
    manifest.spec.features.push_back(f0);
    FeatureDef f1;
    f1.name = "x1";
    f1.tag_index = 1;
    manifest.spec.features.push_back(f1);

    LabelDef l0;
    l0.name = "y";
    manifest.spec.labels.push_back(l0);

    const std::string tmp_weights = "/tmp/test_trainer_weights.pt";

    auto res = trainer.trainAndSave(model, x, y, train_cfg, manifest, tmp_weights);
    CHECK(!res.hasError());
    CHECK(res.value().epochs_completed == 20);
    CHECK(res.value().final_train_loss < 0.1f);
    CHECK(res.value().epoch_losses.front() > res.value().epoch_losses.back()); // loss decreased

    // 4. Verify manifest weights integrity
    CHECK(!manifest.weights_sha256.empty());
    CHECK(!manifest.validateWeightsFile(tmp_weights).hasError());

    // 5. Reload into TorchBackend and verify inference
    TorchBackend backend;
    auto load_res = backend.load(tmp_weights, manifest);
    CHECK(!load_res.hasError());
    if (load_res.hasError()) {
        std::printf("load_res failed: %s\n", load_res.error().c_str());
    }

    // Test input: [1.0, 2.0] -> expected: 2.0 * 1.0 - 3.0 * 2.0 = -4.0
    float test_feat[] = {1.0f, 2.0f};
    auto pred_res = backend.predict(test_feat);
    CHECK(!pred_res.hasError());
    if (!pred_res.hasError()) {
        CHECK(pred_res.value().size() == 1);
        const float pred_val = pred_res.value()[0];
        CHECK(std::abs(pred_val - (-4.0f)) < 0.5f); // Trained model is close to true value
    } else {
        std::printf("predict failed: %s\n", pred_res.error().c_str());
    }

    std::filesystem::remove(tmp_weights);

    if (g_failures == 0) {
        std::printf("[trainer_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[trainer_test] %d tests FAILED.\n", g_failures);
    return 1;
}
