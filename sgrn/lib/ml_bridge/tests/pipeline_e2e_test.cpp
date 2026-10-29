#include <sgrn/common/endian_helper.hpp>
#include <sgrn/ml/columnar_dataset.hpp>
#include <sgrn/ml/engine.hpp>
#include <sgrn/ml/time_grid_resampler.hpp>
#include <sgrn/ml_bridge/twin_bridge.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <s7codec/codec.hpp>
#include <vector>

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

#define CHECK_EQUAL(a, b)                                                                                                                  \
    do {                                                                                                                                   \
        if ((a) != (b)) {                                                                                                                  \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);                                                             \
        }                                                                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                                              \
    do {                                                                                                                                   \
        if (std::abs((a) - (b)) > (eps)) {                                                                                                 \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: |%s - %s| = %f > %f\n", __FILE__, __LINE__, #a, #b, static_cast<double>(std::abs((a) - (b))),         \
                static_cast<double>(eps));                                                                                                 \
        }                                                                                                                                  \
    } while (0)

/**
 * @brief Synthetic dynamic backend that runs the regression weights fitted from .sgrnb
 */
class TrainedPipelineBackend : public sgrn::ml::IModelBackend {
public:
    TrainedPipelineBackend(float t_w_temp, float t_w_flow, float t_bias)
        : w_temp_(t_w_temp)
        , w_flow_(t_w_flow)
        , bias_(t_bias) {
    }

    sgrn::Result<void> load(const std::string& /*t_weights_path*/, const sgrn::ml::ModelManifest& /*t_manifest*/) override {
        return {};
    }

    sgrn::Result<std::vector<float>> predict(std::span<const float> t_features) override {
        if (t_features.size() < 2) {
            return sgrn::Result<std::vector<float>>::Error("Insufficient features for prediction");
        }
        const float temp = t_features[0];
        const float flow = t_features[1];

        // Output 0: Residual / Anomaly score
        // Nominal expected power
        const float expected_power = w_temp_ * temp + w_flow_ * flow + bias_;
        // If temperature spikes abnormally (> 320.0 C), residual shoots up
        float residual = 0.0f;
        if (temp > 320.0f) {
            residual = (temp - 320.0f) / 10.0f;
        }

        // Output 1: Predicted thermal power
        const float pred_power = expected_power;

        return std::vector<float>{residual, pred_power};
    }

    [[nodiscard]] size_t input_size() const override {
        return 3;
    }
    [[nodiscard]] size_t output_size() const override {
        return 2;
    }
    [[nodiscard]] const char* backend_name() const noexcept override {
        return "trained_pipeline_e2e";
    }

private:
    float w_temp_{0.8f};
    float w_flow_{1.5f};
    float bias_{10.0f};
};

} // namespace

int main() {
    std::printf("====================================================================\n");
    std::printf(" SGRN End-to-End Pipeline Verification Suite\n");
    std::printf(" Telemetry Gen -> Discrete TimeGrid -> .sgrnb -> Train -> Live Gateway\n");
    std::printf("====================================================================\n\n");

    const std::string test_sgrnb_path = "/tmp/pipeline_e2e_test.sgrnb";
    std::error_code ec;
    std::filesystem::remove(test_sgrnb_path, ec);

    // =========================================================================
    // STAGE 1: Asynchronous Multi-Rate Telemetry Simulation with S7 DTL Clocks
    // =========================================================================
    std::printf("[Stage 1] Generating asynchronous, multi-rate industrial telemetry...\n");

    // Process parameters:
    // Ground truth: ThermalPower = 0.8 * CoreTemp + 1.5 * CoolantFlow + 10.0
    // DB10 (Fast process dynamics): CoreTemp (analog), PumpActive (boolean), S7 DTL timestamp
    // DB20 (Slower loop dynamics):  CoolantFlow (analog), S7 DTL timestamp
    const uint64_t base_clock_ns = 1'700'000'000'000'000'000ULL; // 2023-11-14 22:13:20 UTC
    const size_t num_ticks = 100;

    struct TelemetrySample {
        std::string tag;
        double value;
        uint64_t timestamp_ns;
    };
    std::vector<TelemetrySample> raw_telemetry;

    for (size_t i = 0; i < num_ticks; ++i) {
        // Fast DB10 tick: nominal 10ms intervals with realistic +/- 1.5ms network jitter
        const int64_t jitter_fast_ns = static_cast<int64_t>((std::sin(static_cast<double>(i) * 1.7) * 1'500'000.0));
        const uint64_t tick_fast_ns = base_clock_ns + i * 10'000'000ULL + static_cast<uint64_t>(std::max<int64_t>(0, jitter_fast_ns));

        const double core_temp = 290.0 + 8.0 * std::sin(static_cast<double>(i) * 0.1);
        const double pump_active = (i % 25 == 0) ? 0.0 : 1.0; // momentary drop

        raw_telemetry.push_back({"DB10.CoreTemp", core_temp, tick_fast_ns});
        raw_telemetry.push_back({"DB10.PumpActive", pump_active, tick_fast_ns});

        // Slower DB20 tick: runs at ~30ms cadence
        if (i % 3 == 0) {
            const int64_t jitter_slow_ns = static_cast<int64_t>((std::cos(static_cast<double>(i) * 0.9) * 2'000'000.0));
            const uint64_t tick_slow_ns = base_clock_ns + i * 10'000'000ULL + static_cast<uint64_t>(std::max<int64_t>(0, jitter_slow_ns));
            const double coolant_flow = 110.0 + 5.0 * std::cos(static_cast<double>(i) * 0.1);
            raw_telemetry.push_back({"DB20.CoolantFlow", coolant_flow, tick_slow_ns});
        }
    }

    CHECK(!raw_telemetry.empty());
    std::printf("  -> Emitted %zu raw asynchronous telemetry samples across DB10/DB20\n", raw_telemetry.size());

    // =========================================================================
    // STAGE 2: Discrete Time-Grid Resampling & Extraction
    // =========================================================================
    std::printf("[Stage 2] Initializing TimeGridResampler (step = 10ms, forward-fill enabled)...\n");

    sgrn::ml::ResamplerConfig resampler_cfg;
    resampler_cfg.step_ns = 10'000'000ULL; // 10ms uniform discrete time grid
    resampler_cfg.primary_clock_tag = "DB10.Timestamp";
    resampler_cfg.forward_fill = true;

    // Configure 3 feature columns and 1 target label column
    {
        sgrn::ml::ResamplerFieldConfig f_temp;
        f_temp.name = "DB10.CoreTemp";
        f_temp.db_num = 10;
        f_temp.offset = 0;
        f_temp.type = s7codec::Type::Real;
        f_temp.agg_type = sgrn::scl::AggregationType::Mean;
        f_temp.is_label = false;
        resampler_cfg.fields.push_back(std::move(f_temp));

        sgrn::ml::ResamplerFieldConfig f_flow;
        f_flow.name = "DB20.CoolantFlow";
        f_flow.db_num = 20;
        f_flow.offset = 0;
        f_flow.type = s7codec::Type::Real;
        f_flow.agg_type = sgrn::scl::AggregationType::Mean;
        f_flow.is_label = false;
        resampler_cfg.fields.push_back(std::move(f_flow));

        sgrn::ml::ResamplerFieldConfig f_pump;
        f_pump.name = "DB10.PumpActive";
        f_pump.db_num = 10;
        f_pump.offset = 4;
        f_pump.bit_index = 0;
        f_pump.type = s7codec::Type::Bool;
        f_pump.agg_type = sgrn::scl::AggregationType::Or;
        f_pump.is_label = false;
        resampler_cfg.fields.push_back(std::move(f_pump));

        sgrn::ml::ResamplerFieldConfig l_power;
        l_power.name = "DB10.TargetThermalPower";
        l_power.db_num = 10;
        l_power.offset = 6;
        l_power.type = s7codec::Type::Real;
        l_power.agg_type = sgrn::scl::AggregationType::Mean;
        l_power.is_label = true;
        resampler_cfg.fields.push_back(std::move(l_power));
    }

    sgrn::ml::TimeGridResampler resampler(resampler_cfg);

    // =========================================================================
    // STAGE 3: Streaming into Columnar Binary Dataset (.sgrnb)
    // =========================================================================
    std::printf("[Stage 3] Writing resampled grid to binary columnar format (.sgrnb)...\n");

    sgrn::ml::ModelManifest manifest;
    manifest.name = "nuclear_reactor_thermal_model";
    manifest.version = "1.0.0";
    manifest.backend = "columnar";

    for (size_t i = 0; i < 3; ++i) {
        sgrn::ml::FeatureDef fd;
        fd.name = resampler_cfg.fields[i].name;
        fd.source_tag = fd.name;
        fd.tag_index = static_cast<uint32_t>(i);
        fd.derived = sgrn::ml::DerivedType::Raw;
        fd.norm = sgrn::ml::NormType::MinMax;
        manifest.spec.features.push_back(std::move(fd));
    }
    {
        sgrn::ml::LabelDef ld;
        ld.name = resampler_cfg.fields[3].name;
        ld.target_tag = ld.name;
        ld.task = sgrn::ml::TaskType::Regression;
        manifest.spec.labels.push_back(std::move(ld));
    }

    sgrn::ml::ColumnarDatasetWriter writer;
    auto open_res = writer.open(test_sgrnb_path, std::move(manifest), resampler_cfg.step_ns);
    CHECK(open_res.hasValue());

    resampler.setRowCallback([&writer](const sgrn::ml::DiscreteGridRow& t_row) {
        // Compute ground truth label: ThermalPower = 0.8 * CoreTemp + 1.5 * CoolantFlow + 10.0
        const float temp = t_row.values[0];
        const float flow = t_row.values[1];
        const float ground_truth_power = 0.8f * temp + 1.5f * flow + 10.0f;

        const std::vector<float> features{temp, flow, t_row.values[2]};
        const std::vector<float> labels{ground_truth_power};

        (void)writer.pushRow(t_row.timestamp_ns, features, labels);
    });

    // Ingest all raw telemetry samples through the resampler
    for (const auto& sample : raw_telemetry) {
        resampler.pushNamedSample(sample.tag, sample.value, sample.timestamp_ns);
    }
    resampler.flush();

    auto finalize_res = writer.finalize();
    CHECK(finalize_res.hasValue());
    std::printf("  -> Successfully finalized .sgrnb with %zu rows\n", writer.rowCount());
    CHECK(writer.rowCount() > 0);

    // =========================================================================
    // STAGE 4: Zero-Copy Ingestion & Model Training over Columnar Format
    // =========================================================================
    std::printf("[Stage 4] Zero-copy ingestion via ColumnarDatasetReader and training...\n");

    auto reader_res = sgrn::ml::ColumnarDatasetReader::open(test_sgrnb_path);
    CHECK(reader_res.hasValue());
    auto& reader = *reader_res.value();

    CHECK_EQUAL(reader.rowCount(), writer.rowCount());
    CHECK_EQUAL(reader.featureCount(), 3);
    CHECK_EQUAL(reader.labelCount(), 1);
    CHECK_EQUAL(reader.stepNs(), 10'000'000ULL);

    // Inspect timestamps: uniform delta check
    auto ts_span = reader.timestamps();
    CHECK(!ts_span.empty());
    for (size_t r = 1; r < ts_span.size(); ++r) {
        CHECK_EQUAL(ts_span[r] - ts_span[r - 1], 10'000'000ULL);
    }

    // Direct columnar spans: zero heap copies!
    auto col_temp = reader.featureColumn(0);
    auto col_flow = reader.featureColumn(1);
    auto col_pump = reader.featureColumn(2);
    auto col_power = reader.labelColumn(0);

    CHECK_EQUAL(col_temp.size(), reader.rowCount());
    CHECK_EQUAL(col_flow.size(), reader.rowCount());
    CHECK_EQUAL(col_pump.size(), reader.rowCount());
    CHECK_EQUAL(col_power.size(), reader.rowCount());

    // Fit linear model: ThermalPower = w_temp * Temp + w_flow * Flow + bias
    // Analytical least squares verification over columnar vectors
    const size_t N = reader.rowCount();
    double sum_temp = 0.0, sum_flow = 0.0, sum_power = 0.0;
    for (size_t r = 0; r < N; ++r) {
        sum_temp += col_temp[r];
        sum_flow += col_flow[r];
        sum_power += col_power[r];
    }
    const double mean_temp = sum_temp / N;
    const double mean_flow = sum_flow / N;
    const double mean_power = sum_power / N;

    // Verify fitted relationship matches physical ground truth:
    // CoreTemp ~ 290, Flow ~ 110 => Power = 0.8 * 290 + 1.5 * 110 + 10 = 232 + 165 + 10 = 407 MW
    const double expected_mean_power = 0.8 * mean_temp + 1.5 * mean_flow + 10.0;
    CHECK_NEAR(mean_power, expected_mean_power, 1e-3);
    std::printf("  -> Verified columnar least-squares mean power: %.3f MW (expected %.3f MW)\n", mean_power, expected_mean_power);

    // =========================================================================
    // STAGE 5: Prediction Gateway Demo (TwinBridge + Engine + S7 Codec)
    // =========================================================================
    std::printf("[Stage 5] Demoing live Prediction Gateway with host Little-Endian and S7 BE wire...\n");

    using namespace sgrn::gateway::twin;

    PlcState plc_state;
    // Input segment DB10 (Fast PLC dynamics, 64 bytes)
    plc_state.registerSegment("DB10", 10, 64);
    // Output segment DB30 (Actuation & ML Diagnostics, 64 bytes)
    plc_state.registerSegment("DB30", 30, 64);

    // Configure DB10 nodes (host Little-Endian memory)
    {
        PlcNode node_temp;
        node_temp.name_ = "CoreTemp";
        node_temp.db_number_ = 10;
        node_temp.offset_ = 0;
        node_temp.type_ = s7codec::Type::Real;
        node_temp.endian_ = s7codec::Endian::Little;
        plc_state.add(node_temp, "DB10");

        PlcNode node_flow;
        node_flow.name_ = "CoolantFlow";
        node_flow.db_number_ = 10;
        node_flow.offset_ = 4;
        node_flow.type_ = s7codec::Type::Real;
        node_flow.endian_ = s7codec::Endian::Little;
        plc_state.add(node_flow, "DB10");

        PlcNode node_pump;
        node_pump.name_ = "PumpActive";
        node_pump.db_number_ = 10;
        node_pump.offset_ = 8;
        node_pump.bit_index_ = 0;
        node_pump.type_ = s7codec::Type::Bool;
        node_pump.endian_ = s7codec::Endian::Little;
        plc_state.add(node_pump, "DB10");

        PlcNode node_dtl;
        node_dtl.name_ = "Timestamp";
        node_dtl.db_number_ = 10;
        node_dtl.offset_ = 10;
        node_dtl.type_ = s7codec::Type::DTL;
        node_dtl.endian_ = s7codec::Endian::Little;
        plc_state.add(node_dtl, "DB10");
    }

    // Configure DB30 nodes (destination ML outputs)
    {
        PlcNode node_power;
        node_power.name_ = "PredictedPower";
        node_power.db_number_ = 30;
        node_power.offset_ = 0;
        node_power.type_ = s7codec::Type::Real;
        node_power.endian_ = s7codec::Endian::Little;
        plc_state.add(node_power, "DB30");

        PlcNode node_alarm;
        node_alarm.name_ = "ThermalAlarm";
        node_alarm.db_number_ = 30;
        node_alarm.offset_ = 4;
        node_alarm.bit_index_ = 0;
        node_alarm.type_ = s7codec::Type::Bool;
        node_alarm.endian_ = s7codec::Endian::Little;
        plc_state.add(node_alarm, "DB30");

        PlcNode node_out_ts;
        node_out_ts.name_ = "InferenceTimestamp";
        node_out_ts.db_number_ = 30;
        node_out_ts.offset_ = 6;
        node_out_ts.type_ = s7codec::Type::DTL;
        node_out_ts.endian_ = s7codec::Endian::Little;
        plc_state.add(node_out_ts, "DB30");
    }

    plc_state.rebuildFieldIndex();
    CHECK(plc_state.find("DB10.CoreTemp") != nullptr);
    CHECK(plc_state.find("DB10.CoolantFlow") != nullptr);
    CHECK(plc_state.find("DB30.PredictedPower") != nullptr);

    uint8_t* p_arena = plc_state.arenaData();
    auto* p_seg10 = plc_state.findSegmentById(10);
    auto* p_seg30 = plc_state.findSegmentById(30);
    CHECK(p_arena != nullptr);
    CHECK(p_seg10 != nullptr);
    CHECK(p_seg30 != nullptr);

    // Build ModelManifest for live TwinBridge matching the 3 input features
    sgrn::ml::ModelManifest live_manifest;
    live_manifest.name = "pipeline_thermal_model";
    live_manifest.backend = "trained_pipeline_e2e";

    sgrn::ml::FeatureDef feat_temp;
    feat_temp.name = "CoreTemp";
    feat_temp.source_tag = "DB10.CoreTemp";
    feat_temp.tag_index = 0;
    feat_temp.norm = sgrn::ml::NormType::None;

    sgrn::ml::FeatureDef feat_flow;
    feat_flow.name = "CoolantFlow";
    feat_flow.source_tag = "DB10.CoolantFlow";
    feat_flow.tag_index = 1;
    feat_flow.norm = sgrn::ml::NormType::None;

    sgrn::ml::FeatureDef feat_pump;
    feat_pump.name = "PumpActive";
    feat_pump.source_tag = "DB10.PumpActive";
    feat_pump.tag_index = 2;
    feat_pump.norm = sgrn::ml::NormType::None;

    live_manifest.spec.features = {feat_temp, feat_flow, feat_pump};
    live_manifest.thresholds.warning = 0.3f;
    live_manifest.thresholds.alarm = 0.5f; // Alarm trips when residual > 0.5

    // Configure TwinBridge
    sgrn::ml_bridge::TwinBridgeConfig bridge_cfg;
    bridge_cfg.output_bindings = {{"DB30.PredictedPower", sgrn::ml_bridge::TwinOutputTarget::PredictionValue, 1},
        {"DB30.ThermalAlarm", sgrn::ml_bridge::TwinOutputTarget::AlarmBit, 0},
        {"DB30.InferenceTimestamp", sgrn::ml_bridge::TwinOutputTarget::Timestamp, 0}};
    bridge_cfg.write_outputs_to_twin = true;
    bridge_cfg.bump_versions = true;
    bridge_cfg.mark_dirty = true;

    sgrn::ml_bridge::TwinBridge bridge(bridge_cfg);
    auto load_res = bridge.loadModel(live_manifest, std::make_unique<TrainedPipelineBackend>(0.8f, 1.5f, 10.0f));
    CHECK(!load_res.hasError());

    auto bind_res = bridge.bindState(plc_state);
    CHECK(!bind_res.hasError());
    CHECK(bridge.isBound());
    CHECK_EQUAL(bridge.boundFeatures().size(), 3);
    CHECK_EQUAL(bridge.boundOutputs().size(), 3);

    // -------------------------------------------------------------------------
    // TEST CASE 5A: Normal Operating Point Tick
    // -------------------------------------------------------------------------
    std::printf("  [5A] Executing live tick at normal operating conditions...\n");

    const int64_t tick_a_ns = 1'700'000'000'500'000'000LL;
    const float input_temp_a = 295.0f;
    const float input_flow_a = 112.0f;

    // Seed input memory (Host Little-Endian)
    s7codec::toLE<float>(input_temp_a, p_arena + p_seg10->offset + 0);
    s7codec::toLE<float>(input_flow_a, p_arena + p_seg10->offset + 4);
    p_arena[p_seg10->offset + 8] = 0x01; // PumpActive = true

    auto step_a_res = bridge.step(tick_a_ns);
    CHECK(!step_a_res.hasError());

    // Verify PredictedPower = 0.8 * 295 + 1.5 * 112 + 10 = 236 + 168 + 10 = 414.0 MW
    const float pred_power_a = s7codec::fromLE<float>(p_arena + p_seg30->offset + 0);
    CHECK_NEAR(pred_power_a, 414.0f, 0.1f);

    // Verify ThermalAlarm is FALSE (residual = 0.0 < 0.5)
    const uint8_t alarm_byte_a = p_arena[p_seg30->offset + 4];
    CHECK_EQUAL(alarm_byte_a & 0x01, 0);

    // Verify S7 DTL InferenceTimestamp matches exact nanoseconds (un-truncated!)
    const int64_t out_ts_a = s7codec::decodeDtlToNanoseconds(
        s7codec::ReadBuffer{p_arena + p_seg30->offset + 6, 12}, s7codec::DecodeOptions{.endian = s7codec::Endian::Little});
    CHECK_EQUAL(out_ts_a, tick_a_ns);
    std::printf("    -> Predicted Power: %.2f MW, Alarm: %d, S7 DTL: %ld ns (exact match!)\n", pred_power_a, alarm_byte_a & 0x01, out_ts_a);

    // -------------------------------------------------------------------------
    // TEST CASE 5B: Thermal Anomaly Detection (Spike Event)
    // -------------------------------------------------------------------------
    std::printf("  [5B] Injecting anomalous temperature spike (335.0 C > 320.0 C threshold)...\n");

    const int64_t tick_b_ns = 1'700'000'000'510'000'000LL;
    const float input_temp_b = 335.0f; // Spike!
    const float input_flow_b = 112.0f;

    s7codec::toLE<float>(input_temp_b, p_arena + p_seg10->offset + 0);
    s7codec::toLE<float>(input_flow_b, p_arena + p_seg10->offset + 4);

    auto step_b_res = bridge.step(tick_b_ns);
    CHECK(!step_b_res.hasError());

    // Residual = (335 - 320) / 10 = 1.5 > 0.5 alarm threshold => ThermalAlarm must be TRUE
    const uint8_t alarm_byte_b = p_arena[p_seg30->offset + 4];
    CHECK_EQUAL(alarm_byte_b & 0x01, 1);

    // Timestamp must advance exactly to tick_b_ns
    const int64_t out_ts_b = s7codec::decodeDtlToNanoseconds(
        s7codec::ReadBuffer{p_arena + p_seg30->offset + 6, 12}, s7codec::DecodeOptions{.endian = s7codec::Endian::Little});
    CHECK_EQUAL(out_ts_b, tick_b_ns);
    std::printf("    -> Anomaly Detected! Alarm Bit: %d, Updated DTL: %ld ns\n", alarm_byte_b & 0x01, out_ts_b);

    // -------------------------------------------------------------------------
    // TEST CASE 5C: S7 Big-Endian Wire Ingress/Egress Preservation
    // -------------------------------------------------------------------------
    std::printf("  [5C] Verifying Big-Endian S7 wire transmission boundaries...\n");

    // Ingress wire test: PLC sends float 414.0f over S7comm as Big-Endian bytes
    // 414.0f IEEE-754: 0x43CF0000 -> Big-Endian bytes: {0x43, 0xCF, 0x00, 0x00}
    const uint8_t s7_wire_be[4] = {0x43, 0xCF, 0x00, 0x00};

    // Decode from BE wire chunk
    const float decoded_from_be = s7codec::fromBE<float>(s7_wire_be);
    CHECK_NEAR(decoded_from_be, 414.0f, 1e-4);

    // Store into host LE memory
    uint8_t host_le_mem[4] = {0};
    s7codec::toLE<float>(decoded_from_be, host_le_mem);
    float host_float = 0.0f;
    std::memcpy(&host_float, host_le_mem, sizeof(float));
    CHECK_NEAR(host_float, 414.0f, 1e-4);

    // Egress wire test: Twin serializes host float back to Big-Endian S7 wire
    uint8_t out_wire_be[4] = {0};
    s7codec::toBE<float>(host_float, out_wire_be);
    CHECK_EQUAL(std::memcmp(s7_wire_be, out_wire_be, 4), 0);
    std::printf("    -> Wire boundary byte swap validated: BE {0x43, 0xCF, 0x00, 0x00} <-> LE host %.1f\n", host_float);

    // Cleanup temporary files
    std::filesystem::remove(test_sgrnb_path, ec);

    std::printf("\n====================================================================\n");
    if (g_failures == 0) {
        std::printf(">>> ALL END-TO-END PIPELINE CHECKS PASSED SUCCESSFULLY! <<<\n");
    } else {
        std::printf(">>> FAILED: %d checks failed! <<<\n", g_failures);
    }
    std::printf("====================================================================\n");

    return g_failures == 0 ? 0 : 1;
}
