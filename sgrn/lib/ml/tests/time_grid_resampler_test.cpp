#include <sgrn/ml/time_grid_resampler.hpp>
#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <cmath>
#include <cstdio>
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
    // 1. Test basic discrete time-grid resampling with typed aggregations
    {
        ResamplerConfig cfg;
        cfg.step_ns = 10'000'000ULL; // 10ms
        cfg.forward_fill = true;

        ResamplerFieldConfig f_speed{
            "DB1.Speed", 1, 0, 0, s7codec::Type::Real, sgrn::scl::AggregationType::Mean, "DB1.Timestamp", false, false};
        ResamplerFieldConfig f_peak{
            "DB1.PeakVib", 1, 4, 0, s7codec::Type::Real, sgrn::scl::AggregationType::Max, "DB1.Timestamp", false, false};
        ResamplerFieldConfig f_trip{"DB1.Trip", 1, 8, 0, s7codec::Type::Bool, sgrn::scl::AggregationType::Or, "DB1.Timestamp", false, true};
        ResamplerFieldConfig f_mode{
            "DB1.Mode", 1, 10, 0, s7codec::Type::Int, sgrn::scl::AggregationType::Last, "DB1.Timestamp", false, false};

        cfg.fields = {f_speed, f_peak, f_trip, f_mode};

        TimeGridResampler resampler(cfg);

        std::vector<DiscreteGridRow> emitted_rows;
        resampler.setRowCallback([&](const DiscreteGridRow& t_row) { emitted_rows.push_back(t_row); });

        // Bucket 0 [100ms, 110ms):
        // Sample at 101ms: Speed=100.0, Peak=2.0, Trip=0, Mode=1
        resampler.pushSample(0, 100.0, 101'000'000ULL);
        resampler.pushSample(1, 2.0, 101'000'000ULL);
        resampler.pushSample(2, 0.0, 101'000'000ULL);
        resampler.pushSample(3, 1.0, 101'000'000ULL);

        // Sample at 105ms (same bucket): Speed=200.0, Peak=8.5 (spike!), Trip=1 (pulse!), Mode=2
        resampler.pushSample(0, 200.0, 105'000'000ULL);
        resampler.pushSample(1, 8.5, 105'000'000ULL);
        resampler.pushSample(2, 1.0, 105'000'000ULL);
        resampler.pushSample(3, 2.0, 105'000'000ULL);

        // Advancing to Bucket 1 [110ms, 120ms):
        // Sample at 112ms: Speed=300.0, Peak=1.0, Trip=0, Mode=3
        resampler.pushSample(0, 300.0, 112'000'000ULL);
        resampler.pushSample(1, 1.0, 112'000'000ULL);
        resampler.pushSample(2, 0.0, 112'000'000ULL);
        resampler.pushSample(3, 3.0, 112'000'000ULL);

        // Advancing with a GAP: skip Bucket 2 [120ms, 130ms), jump directly to Bucket 3 [130ms, 140ms)
        // Sample at 134ms: Speed=400.0
        resampler.pushSample(0, 400.0, 134'000'000ULL);

        resampler.flush();

        // We expect 4 emitted rows:
        // Row 0 @ 100ms
        // Row 1 @ 110ms
        // Row 2 @ 120ms (forward-filled gap)
        // Row 3 @ 130ms
        CHECK(emitted_rows.size() == 4);

        if (emitted_rows.size() >= 4) {
            // Row 0 checks:
            CHECK(emitted_rows[0].timestamp_ns == 100'000'000ULL);
            CHECK(std::abs(emitted_rows[0].values[0] - 150.0f) < 1e-4f); // Mean of 100 and 200 = 150
            CHECK(std::abs(emitted_rows[0].values[1] - 8.5f) < 1e-4f);   // Max of 2.0 and 8.5 = 8.5
            CHECK(emitted_rows[0].values[2] == 1.0f);                    // OR of 0 and 1 = 1
            CHECK(emitted_rows[0].values[3] == 2.0f);                    // Last of 1 and 2 = 2

            // Row 1 checks:
            CHECK(emitted_rows[1].timestamp_ns == 110'000'000ULL);
            CHECK(std::abs(emitted_rows[1].values[0] - 300.0f) < 1e-4f);
            CHECK(std::abs(emitted_rows[1].values[1] - 1.0f) < 1e-4f);
            CHECK(emitted_rows[1].values[2] == 0.0f);
            CHECK(emitted_rows[1].values[3] == 3.0f);

            // Row 2 checks (gap bucket at 120ms, forward-filled from Row 1):
            CHECK(emitted_rows[2].timestamp_ns == 120'000'000ULL);
            CHECK(std::abs(emitted_rows[2].values[0] - 300.0f) < 1e-4f); // Preserves previous speed 300.0
            CHECK(std::abs(emitted_rows[2].values[1] - 1.0f) < 1e-4f);   // Preserves previous peak 1.0
            CHECK(emitted_rows[2].values[2] == 0.0f);
            CHECK(emitted_rows[2].values[3] == 3.0f);

            // Row 3 checks:
            CHECK(emitted_rows[3].timestamp_ns == 130'000'000ULL);
            CHECK(std::abs(emitted_rows[3].values[0] - 400.0f) < 1e-4f);
            // Non-updated fields forward-fill from Row 2
            CHECK(std::abs(emitted_rows[3].values[1] - 1.0f) < 1e-4f);
            CHECK(emitted_rows[3].values[3] == 3.0f);
        }
    }

    // 2. Test makeResamplerConfig with SCL directives
    {
        static const char* kScl = R"(
DATA_BLOCK "Drive" DB 50
#DATASET_STEP(20ms)
VAR
    Clock : DTL #TIMESTAMP_FOR(DB);
    Speed : Real #AGG(MEAN);
    PeakCurrent : Real #AGG(MAX);
    Alarm : Bool #AGG(OR);
    Ignored : Real #TRANSIENT;
END_VAR
END_DATA_BLOCK
)";
        auto parse_res = sgrn::scl::DbSymbolsParser::parseString(kScl);
        CHECK(!parse_res.hasError());
        if (!parse_res.hasError()) {
            auto cfg = makeResamplerConfig(parse_res.value().dbs);
            CHECK(cfg.step_ns == 20'000'000ULL); // 20ms from #DATASET_STEP
            CHECK(cfg.primary_clock_tag == "DB50.Clock");
            // Transient field ignored, clock field ignored from features -> 3 columns remaining
            CHECK(cfg.fields.size() == 3);

            CHECK(cfg.fields[0].name == "DB50.Speed");
            CHECK(cfg.fields[0].agg_type == sgrn::scl::AggregationType::Mean);

            CHECK(cfg.fields[1].name == "DB50.PeakCurrent");
            CHECK(cfg.fields[1].agg_type == sgrn::scl::AggregationType::Max);

            CHECK(cfg.fields[2].name == "DB50.Alarm");
            CHECK(cfg.fields[2].agg_type == sgrn::scl::AggregationType::Or);
            CHECK(cfg.fields[2].is_boolean);
        }
    }

    if (g_failures == 0) {
        std::printf("[time_grid_resampler_test] All tests passed.\n");
        return 0;
    }
    std::printf("[time_grid_resampler_test] %d tests FAILED.\n", g_failures);
    return 1;
}
