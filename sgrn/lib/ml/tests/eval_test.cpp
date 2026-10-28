#include <sgrn/ml/eval.hpp>
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

    // 1. Regression metrics
    {
        float preds[] = {2.0f, 4.0f, 6.0f};
        float targets[] = {1.0f, 4.0f, 7.0f};
        // errors: 1.0, 0.0, -1.0
        // sum_sq_err = 1 + 0 + 1 = 2.0 -> mse = 2/3 = 0.66667, rmse = sqrt(2/3)
        // sum_abs_err = 1 + 0 + 1 = 2.0 -> mae = 2/3
        // max_error = 1.0
        auto res = Evaluator::evaluateRegression(preds, targets);
        CHECK(!res.hasError());
        const auto& m = res.value();
        CHECK(m.sample_count == 3);
        CHECK(std::abs(m.mae - (2.0 / 3.0)) < 1e-4);
        CHECK(std::abs(m.max_error - 1.0) < 1e-4);
        CHECK(std::abs(m.mse - (2.0 / 3.0)) < 1e-4);
        CHECK(std::abs(m.rmse - std::sqrt(2.0 / 3.0)) < 1e-4);
    }

    // 2. Binary classification metrics
    {
        float preds[] = {0.9f, 0.1f, 0.8f, 0.3f};
        float targets[] = {1.0f, 0.0f, 0.0f, 0.0f};
        // pred:  [1, 0, 1, 0]
        // true:  [1, 0, 0, 0]
        // tp = 1, tn = 2, fp = 1, fn = 0
        // accuracy = 3/4 = 0.75
        // precision = 1 / (1 + 1) = 0.5
        // recall = 1 / (1 + 0) = 1.0
        // f1 = 2 * (0.5 * 1.0) / 1.5 = 2/3
        auto res = Evaluator::evaluateBinary(preds, targets, 0.5f);
        CHECK(!res.hasError());
        const auto& m = res.value();
        CHECK(m.tp == 1);
        CHECK(m.tn == 2);
        CHECK(m.fp == 1);
        CHECK(m.fn == 0);
        CHECK(std::abs(m.accuracy - 0.75) < 1e-4);
        CHECK(std::abs(m.precision - 0.5) < 1e-4);
        CHECK(std::abs(m.recall - 1.0) < 1e-4);
        CHECK(std::abs(m.f1 - (2.0 / 3.0)) < 1e-4);
    }

    // 3. Trace comparison
    {
        double sim[] = {10.0, 20.0, 30.0, 40.0};
        double real[] = {10.5, 19.5, 30.2, 39.0};
        auto cmp_res = Evaluator::compareSignals("ReactorPressure", sim, real);
        CHECK(!cmp_res.hasError());
        const auto& cmp = cmp_res.value();
        CHECK(cmp.signal_name == "ReactorPressure");
        CHECK(cmp.length == 4);
        CHECK(cmp.max_abs_dev == 1.0); // at index 3 (40 - 39)
        CHECK(cmp.max_dev_index == 3);
        CHECK(cmp.pearson_correlation > 0.99); // strong positive correlation
    }

    if (g_failures == 0) {
        std::printf("[eval_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[eval_test] %d tests FAILED.\n", g_failures);
    return 1;
}
