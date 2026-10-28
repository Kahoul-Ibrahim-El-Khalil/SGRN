#include <sgrn/ml_torch/mlp.hpp>
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
    using namespace sgrn::ml_torch;

    sgrn::ml::MlpConfig cfg;
    cfg.use_batch_norm = false;
    cfg.layers.push_back({16, "relu", 0.0f});
    cfg.layers.push_back({8, "tanh", 0.0f});

    Mlp model(4, cfg, 2);
    CHECK(model->inputSize() == 4);
    CHECK(model->outputSize() == 2);

    // Forward with batch size 3
    torch::Tensor x = torch::randn({3, 4});
    torch::Tensor y = model->forward(x);

    CHECK(y.dim() == 2);
    CHECK(y.size(0) == 3);
    CHECK(y.size(1) == 2);

    // Gradient check
    torch::Tensor target = torch::ones({3, 2});
    torch::Tensor loss = torch::mse_loss(y, target);
    loss.backward();

    bool has_grad = true;
    for (const auto& p : model->parameters()) {
        if (!p.grad().defined()) {
            has_grad = false;
            break;
        }
    }
    CHECK(has_grad);

    if (g_failures == 0) {
        std::printf("[mlp_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[mlp_test] %d tests FAILED.\n", g_failures);
    return 1;
}
