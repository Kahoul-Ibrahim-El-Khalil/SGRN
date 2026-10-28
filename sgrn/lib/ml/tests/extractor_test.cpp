#include <sgrn/ml/extractor.hpp>
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

    FeatureSpec spec;
    spec.name = "extractor_test_spec";

    // Feature 0: Raw
    FeatureDef f0;
    f0.name = "raw_val";
    f0.tag_index = 0;
    f0.derived = DerivedType::Raw;
    spec.features.push_back(f0);

    // Feature 1: Rate of change (dt in seconds)
    FeatureDef f1;
    f1.name = "roc_val";
    f1.tag_index = 0;
    f1.derived = DerivedType::RateOfChange;
    spec.features.push_back(f1);

    // Feature 2: Delta
    FeatureDef f2;
    f2.name = "delta_val";
    f2.tag_index = 0;
    f2.derived = DerivedType::Delta;
    spec.features.push_back(f2);

    // Feature 3: Moving Average (window = 3)
    FeatureDef f3;
    f3.name = "sma_val";
    f3.tag_index = 0;
    f3.derived = DerivedType::MovingAverage;
    f3.param = 3.0;
    spec.features.push_back(f3);

    // Feature 4: Lag (lag = 2)
    FeatureDef f4;
    f4.name = "lag_val";
    f4.tag_index = 0;
    f4.derived = DerivedType::Lag;
    f4.param = 2.0;
    spec.features.push_back(f4);

    FeatureExtractor ext(spec);

    // Step 1: t = 1.0s (1,000,000,000 ns), val = 10.0
    double raw1[] = {10.0};
    auto out1 = ext.extract(1'000'000'000ULL, raw1);
    CHECK(!out1.hasError());
    CHECK(out1.value().size() == 5);
    CHECK(std::abs(out1.value()[0] - 10.0f) < 1e-5f); // Raw
    CHECK(std::abs(out1.value()[1] - 0.0f) < 1e-5f);  // RoC initial = 0
    CHECK(std::abs(out1.value()[2] - 0.0f) < 1e-5f);  // Delta initial = 0
    CHECK(std::abs(out1.value()[3] - 10.0f) < 1e-5f); // SMA window(10) = 10
    CHECK(std::abs(out1.value()[4] - 10.0f) < 1e-5f); // Lag fallback initial = 10

    // Step 2: t = 2.0s (2,000,000,000 ns), val = 20.0
    // dt = 1.0s, delta = 10.0 -> RoC = 10.0
    double raw2[] = {20.0};
    auto out2 = ext.extract(2'000'000'000ULL, raw2);
    CHECK(!out2.hasError());
    CHECK(std::abs(out2.value()[0] - 20.0f) < 1e-5f); // Raw
    CHECK(std::abs(out2.value()[1] - 10.0f) < 1e-5f); // RoC = (20 - 10) / 1.0 = 10.0
    CHECK(std::abs(out2.value()[2] - 10.0f) < 1e-5f); // Delta = 20 - 10 = 10.0
    CHECK(std::abs(out2.value()[3] - 15.0f) < 1e-5f); // SMA window(10, 20) = 15.0

    // Step 3: t = 3.0s, val = 30.0
    double raw3[] = {30.0};
    auto out3 = ext.extract(3'000'000'000ULL, raw3);
    CHECK(!out3.hasError());
    CHECK(std::abs(out3.value()[0] - 30.0f) < 1e-5f); // Raw
    CHECK(std::abs(out3.value()[1] - 10.0f) < 1e-5f); // RoC = 10.0
    CHECK(std::abs(out3.value()[2] - 10.0f) < 1e-5f); // Delta = 10.0
    CHECK(std::abs(out3.value()[3] - 20.0f) < 1e-5f); // SMA window(10, 20, 30) = 20.0
    CHECK(std::abs(out3.value()[4] - 10.0f) < 1e-5f); // Lag (2 steps back) = 10.0

    // Step 4: t = 4.0s, val = 40.0
    double raw4[] = {40.0};
    auto out4 = ext.extract(4'000'000'000ULL, raw4);
    CHECK(!out4.hasError());
    CHECK(std::abs(out4.value()[3] - 30.0f) < 1e-5f); // SMA window(20, 30, 40) = 30.0
    CHECK(std::abs(out4.value()[4] - 20.0f) < 1e-5f); // Lag (2 steps back) = 20.0

    // Test reset()
    ext.reset();
    auto out_reset = ext.extract(1'000'000'000ULL, raw1);
    CHECK(!out_reset.hasError());
    CHECK(std::abs(out_reset.value()[1] - 0.0f) < 1e-5f); // RoC after reset = 0

    if (g_failures == 0) {
        std::printf("[extractor_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[extractor_test] %d tests FAILED.\n", g_failures);
    return 1;
}
