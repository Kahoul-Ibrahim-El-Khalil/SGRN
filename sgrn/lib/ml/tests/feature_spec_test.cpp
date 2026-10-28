#include <sgrn/ml/feature_spec.hpp>
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

    // 1. Normalization tests
    {
        FeatureDef minmax_feat;
        minmax_feat.norm = NormType::MinMax;
        minmax_feat.min_val = 10.0;
        minmax_feat.max_val = 20.0;

        CHECK(std::abs(minmax_feat.normalize(10.0f) - 0.0f) < 1e-5f);
        CHECK(std::abs(minmax_feat.normalize(15.0f) - 0.5f) < 1e-5f);
        CHECK(std::abs(minmax_feat.normalize(20.0f) - 1.0f) < 1e-5f);

        FeatureDef zscore_feat;
        zscore_feat.norm = NormType::ZScore;
        zscore_feat.mean_val = 50.0;
        zscore_feat.std_val = 10.0;

        CHECK(std::abs(zscore_feat.normalize(50.0f) - 0.0f) < 1e-5f);
        CHECK(std::abs(zscore_feat.normalize(60.0f) - 1.0f) < 1e-5f);
        CHECK(std::abs(zscore_feat.normalize(40.0f) - (-1.0f)) < 1e-5f);
    }

    // 2. FeatureSpec JSON roundtrip
    {
        FeatureSpec spec;
        spec.name = "vibration_pipeline";
        spec.version = "1.2.0";

        FeatureDef f1;
        f1.name = "vib_x";
        f1.source_tag = "DB1.VIB_X";
        f1.tag_index = 0;
        f1.derived = DerivedType::Raw;
        f1.norm = NormType::MinMax;
        f1.min_val = 0.0;
        f1.max_val = 100.0;
        spec.features.push_back(f1);

        FeatureDef f2;
        f2.name = "vib_roc";
        f2.source_tag = "DB1.VIB_X";
        f2.tag_index = 0;
        f2.derived = DerivedType::RateOfChange;
        f2.norm = NormType::None;
        spec.features.push_back(f2);

        LabelDef l1;
        l1.name = "bearing_failure";
        l1.target_tag = "DB1.FAILURE";
        l1.task = TaskType::BinaryClassification;
        l1.threshold = 0.85;
        l1.classes = {"normal", "failure"};
        spec.labels.push_back(l1);

        CHECK(spec.inputSize() == 2);
        CHECK(spec.outputSize() == 1);
        CHECK(spec.findFeatureIndex("vib_x") == 0);
        CHECK(spec.findFeatureIndex("vib_roc") == 1);
        CHECK(spec.findFeatureIndex("nonexistent") == -1);
        CHECK(spec.findLabelIndex("bearing_failure") == 0);

        const std::string json = spec.toJson();
        auto spec2_res = FeatureSpec::fromJson(json);
        CHECK(!spec2_res.hasError());

        const auto& spec2 = spec2_res.value();
        CHECK(spec2.name == "vibration_pipeline");
        CHECK(spec2.version == "1.2.0");
        CHECK(spec2.features.size() == 2);
        CHECK(spec2.features[0].name == "vib_x");
        CHECK(spec2.features[0].norm == NormType::MinMax);
        CHECK(spec2.features[0].min_val == 0.0);
        CHECK(spec2.features[0].max_val == 100.0);
        CHECK(spec2.features[1].name == "vib_roc");
        CHECK(spec2.features[1].derived == DerivedType::RateOfChange);

        CHECK(spec2.labels.size() == 1);
        CHECK(spec2.labels[0].name == "bearing_failure");
        CHECK(spec2.labels[0].classes.size() == 2);
        CHECK(spec2.labels[0].classes[1] == "failure");
    }

    if (g_failures == 0) {
        std::printf("[feature_spec_test] All tests PASSED.\n");
        return 0;
    }
    std::printf("[feature_spec_test] %d tests FAILED.\n", g_failures);
    return 1;
}
