#include <sgrn/ml/columnar_dataset.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using namespace sgrn::ml;

int main() {
    const std::string test_file = "/tmp/test_dataset.sgrnb";
    if (std::filesystem::exists(test_file)) {
        std::filesystem::remove(test_file);
    }

    ModelManifest manifest;
    manifest.name = "test_columnar_model";
    manifest.backend = "torch";

    FeatureDef f0;
    f0.name = "speed";
    f0.source_tag = "DB10.Speed";
    f0.norm = NormType::MinMax;

    FeatureDef f1;
    f1.name = "pressure";
    f1.source_tag = "DB10.Pressure";
    f1.norm = NormType::MinMax;

    manifest.spec.features = {f0, f1};

    LabelDef l0;
    l0.name = "target_speed";
    l0.target_tag = "DB10.TargetSpeed";
    l0.task = TaskType::Regression;

    manifest.spec.labels = {l0};

    // 1. Write 500 rows
    {
        ColumnarDatasetWriter writer;
        auto open_res = writer.open(test_file, manifest, 10'000'000ULL);
        CHECK(!open_res.hasError());

        uint64_t ts = 1'000'000'000ULL;
        for (size_t r = 0; r < 500; ++r) {
            float feats[2] = {static_cast<float>(r * 2.0f), static_cast<float>(r * 0.5f)};
            float labs[1] = {static_cast<float>(r * 2.5f)};
            auto push_res = writer.pushRow(ts, feats, labs);
            CHECK(!push_res.hasError());
            ts += 10'000'000ULL;
        }

        CHECK(writer.rowCount() == 500);
        auto fin_res = writer.finalize();
        CHECK(!fin_res.hasError());
    }

    CHECK(std::filesystem::exists(test_file));
    CHECK(std::filesystem::exists("/tmp/test_dataset_manifest.json"));

    // 2. Read back with ColumnarDatasetReader via zero-copy mmap
    {
        auto reader_res = ColumnarDatasetReader::open(test_file);
        CHECK(!reader_res.hasError());
        if (!reader_res.hasError()) {
            auto reader = std::move(reader_res.value());
            CHECK(reader->rowCount() == 500);
            CHECK(reader->featureCount() == 2);
            CHECK(reader->labelCount() == 1);
            CHECK(reader->stepNs() == 10'000'000ULL);
            CHECK(reader->manifest().name == "test_columnar_model");

            auto ts_span = reader->timestamps();
            CHECK(ts_span.size() == 500);
            CHECK(ts_span[0] == 1'000'000'000ULL);
            CHECK(ts_span[1] == 1'010'000'000ULL);
            CHECK(ts_span[499] == 1'000'000'000ULL + 499 * 10'000'000ULL);

            auto feat0 = reader->featureColumn(0);
            CHECK(feat0.size() == 500);
            CHECK(feat0[0] == 0.0f);
            CHECK(feat0[1] == 2.0f);
            CHECK(feat0[100] == 200.0f);

            auto feat1 = reader->featureColumn(1);
            CHECK(feat1.size() == 500);
            CHECK(feat1[0] == 0.0f);
            CHECK(feat1[1] == 0.5f);
            CHECK(feat1[100] == 50.0f);

            auto lab0 = reader->labelColumn(0);
            CHECK(lab0.size() == 500);
            CHECK(lab0[0] == 0.0f);
            CHECK(lab0[1] == 2.5f);
            CHECK(lab0[100] == 250.0f);

            // Verify min/max/mean/std were computed and stored in manifest
            const auto& m_feat0 = reader->manifest().spec.features[0];
            CHECK(m_feat0.min_val == 0.0);
            CHECK(m_feat0.max_val == 499.0 * 2.0);
        }
    }

    if (g_failures == 0) {
        std::printf("[columnar_dataset_test] All tests passed.\n");
        return 0;
    }
    std::printf("[columnar_dataset_test] %d tests FAILED.\n", g_failures);
    return 1;
}
