#include <sgrn/ml/twin_bridge.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>

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

class SimplePredictBackend : public sgrn::ml::IModelBackend {
public:
    sgrn::Result<void> load(const std::string& t_weights_path, const sgrn::ml::ModelManifest& t_manifest) override {
        (void)t_weights_path;
        (void)t_manifest;
        return {};
    }

    sgrn::Result<std::vector<float>> predict(std::span<const float> t_features) override {
        // Output 0: anomaly raw score (average of normalized inputs) -> evaluated by PredictionEngine against thresholds
        // Output 1: predicted speed (scaled input 0 * 3000.0)
        // Output 2: echo timestamp
        float speed_norm = t_features.empty() ? 0.0f : t_features[0];
        float sum = 0.0f;
        for (size_t i = 0; i < std::min<size_t>(3, t_features.size()); ++i) {
            sum += t_features[i];
        }
        float avg = sum / 3.0f;
        float ts = t_features.size() > 3 ? t_features[3] : 0.0f;
        return std::vector<float>{avg, speed_norm * 3000.0f, ts};
    }

    size_t input_size() const override {
        return 4;
    }
    size_t output_size() const override {
        return 3;
    }
    const char* backend_name() const noexcept override {
        return "simple_predict";
    }
};

} // namespace

int main() {
    using namespace sgrn::ml;
    using namespace sgrn::gateway::twin;

    // 1. Setup PlcState with input DB100 and output DB200
    PlcState state;
    state.registerSegment("DB100", 100, 64);
    state.registerSegment("DB200", 200, 64);

    // Input fields: DB100.Speed (REAL), DB100.Pressure (REAL), DB100.Running (BOOL)
    {
        PlcNode node_speed;
        node_speed.name_ = "Speed";
        node_speed.db_number_ = 100;
        node_speed.offset_ = 0;
        node_speed.type_ = s7codec::Type::Real;
        node_speed.endian_ = s7codec::Endian::Big;
        state.add(node_speed, "DB100");

        PlcNode node_press;
        node_press.name_ = "Pressure";
        node_press.db_number_ = 100;
        node_press.offset_ = 4;
        node_press.type_ = s7codec::Type::Real;
        node_press.endian_ = s7codec::Endian::Big;
        state.add(node_press, "DB100");

        PlcNode node_running;
        node_running.name_ = "Running";
        node_running.db_number_ = 100;
        node_running.offset_ = 8;
        node_running.bit_index_ = 0;
        node_running.type_ = s7codec::Type::Bool;
        node_running.endian_ = s7codec::Endian::Big;
        state.add(node_running, "DB100");

        PlcNode node_dtl;
        node_dtl.name_ = "Timestamp";
        node_dtl.db_number_ = 100;
        node_dtl.offset_ = 10;
        node_dtl.type_ = s7codec::Type::DTL;
        node_dtl.endian_ = s7codec::Endian::Big;
        state.add(node_dtl, "DB100");
    }

    // Output fields: DB200.PredictedSpeed (REAL), DB200.AnomalyScore (REAL), DB200.Alarm (BOOL), DB200.InferenceTimestamp (DTL)
    {
        PlcNode node_pred;
        node_pred.name_ = "PredictedSpeed";
        node_pred.db_number_ = 200;
        node_pred.offset_ = 0;
        node_pred.type_ = s7codec::Type::Real;
        node_pred.endian_ = s7codec::Endian::Big;
        state.add(node_pred, "DB200");

        PlcNode node_score;
        node_score.name_ = "AnomalyScore";
        node_score.db_number_ = 200;
        node_score.offset_ = 4;
        node_score.type_ = s7codec::Type::Real;
        node_score.endian_ = s7codec::Endian::Big;
        state.add(node_score, "DB200");

        PlcNode node_alarm;
        node_alarm.name_ = "Alarm";
        node_alarm.db_number_ = 200;
        node_alarm.offset_ = 8;
        node_alarm.bit_index_ = 0;
        node_alarm.type_ = s7codec::Type::Bool;
        node_alarm.endian_ = s7codec::Endian::Big;
        state.add(node_alarm, "DB200");

        PlcNode node_out_dtl;
        node_out_dtl.name_ = "InferenceTimestamp";
        node_out_dtl.db_number_ = 200;
        node_out_dtl.offset_ = 10;
        node_out_dtl.type_ = s7codec::Type::DTL;
        node_out_dtl.endian_ = s7codec::Endian::Big;
        state.add(node_out_dtl, "DB200");
    }

    state.rebuildFieldIndex();

    CHECK(state.find("DB100.Speed") != nullptr);
    CHECK(state.find("DB100.Pressure") != nullptr);
    CHECK(state.find("DB100.Running") != nullptr);
    CHECK(state.find("DB200.PredictedSpeed") != nullptr);

    // 2. Populate DB100 initial raw values in the arena
    auto* p_seg100 = state.findSegmentById(100);
    CHECK(p_seg100 != nullptr);

    uint8_t* p_arena = state.arenaData();
    CHECK(p_arena != nullptr);

    // Speed = 1500.0f (BE)
    float speed_val = 1500.0f;
    s7codec::toBE<float>(speed_val, p_arena + p_seg100->offset + 0);

    // Pressure = 5.0f (BE)
    float press_val = 5.0f;
    s7codec::toBE<float>(press_val, p_arena + p_seg100->offset + 4);

    // Running = true (bit 0 = 1)
    p_arena[p_seg100->offset + 8] = 0x01;

    // Timestamp DTL = 2024-03-18 19:30:00.500000000 (1710790200500000000 ns)
    const int64_t sample_dtl_ns = 1710790200'500'000'000LL;
    s7codec::encodeDtl(sample_dtl_ns, s7codec::WriteBuffer{p_arena + p_seg100->offset + 10, 12}, s7codec::Endian::Big);

    // 3. Configure ModelManifest and FeatureSpec
    ModelManifest manifest;
    manifest.name = "twin_test_model";
    manifest.backend = "simple_predict";

    FeatureDef f0;
    f0.name = "speed";
    f0.source_tag = "DB100.Speed";
    f0.tag_index = 0;
    f0.norm = NormType::MinMax;
    f0.min_val = 0.0;
    f0.max_val = 3000.0;
    manifest.spec.features.push_back(f0);

    FeatureDef f1;
    f1.name = "pressure";
    f1.source_tag = "DB100.Pressure";
    f1.tag_index = 1;
    f1.norm = NormType::MinMax;
    f1.min_val = 0.0;
    f1.max_val = 10.0;
    manifest.spec.features.push_back(f1);

    FeatureDef f2;
    f2.name = "running";
    f2.source_tag = "DB100.Running";
    f2.tag_index = 2;
    f2.norm = NormType::None;
    manifest.spec.features.push_back(f2);

    FeatureDef f3;
    f3.name = "timestamp";
    f3.source_tag = "DB100.Timestamp";
    f3.tag_index = 3;
    f3.norm = NormType::None;
    manifest.spec.features.push_back(f3);

    manifest.thresholds.warning = 0.6f;
    manifest.thresholds.alarm = 0.8f;

    // 4. Configure TwinBridge
    TwinBridgeConfig bridge_cfg;
    bridge_cfg.output_bindings = {{"DB200.PredictedSpeed", TwinOutputTarget::PredictionValue, 1},
        {"DB200.AnomalyScore", TwinOutputTarget::AnomalyScore, 0}, {"DB200.Alarm", TwinOutputTarget::AlarmBit, 0},
        {"DB200.InferenceTimestamp", TwinOutputTarget::Timestamp, 0}};
    bridge_cfg.write_outputs_to_twin = true;
    bridge_cfg.bump_versions = true;
    bridge_cfg.mark_dirty = true;

    TwinBridge bridge(bridge_cfg);
    auto load_res = bridge.loadModel(manifest, std::make_unique<SimplePredictBackend>());
    CHECK(!load_res.hasError());

    auto bind_res = bridge.bindState(state);
    CHECK(!bind_res.hasError());
    CHECK(bridge.isBound());
    CHECK(bridge.boundFeatures().size() == 4);
    CHECK(bridge.boundOutputs().size() == 4);

    // 5. Run inference tick via step()
    auto* p_seg200 = state.findSegmentById(200);
    p_seg200->getAndClearDirty();

    auto step_res = bridge.step(sample_dtl_ns);
    CHECK(!step_res.hasError());

    const auto& pred = step_res.value();
    CHECK(pred.outputs.size() == 3);

    // Speed input was 1500 (normalized = 0.5), predicted speed should be 0.5 * 3000 = 1500
    CHECK(std::abs(pred.outputs[1] - 1500.0f) < 1e-3);

    // 6. Verify written outputs in DB200 arena memory
    CHECK(p_seg200->is_dirty_.load());

    // Read back PredictedSpeed from DB200
    float out_speed = s7codec::fromBE<float>(p_arena + p_seg200->offset + 0);
    CHECK(std::abs(out_speed - 1500.0f) < 1e-3);

    // Read back AnomalyScore from DB200
    float out_score = s7codec::fromBE<float>(p_arena + p_seg200->offset + 4);
    CHECK(out_score > 0.0f);

    // Read back Alarm bit from DB200
    uint8_t alarm_byte = p_arena[p_seg200->offset + 8];
    // Since inputs (0.5, 0.5, 1.0) avg = 0.6667 > warning (0.6) but < alarm (0.8), alarm bit should be 0
    CHECK((alarm_byte & 0x01) == 0);

    // Read back InferenceTimestamp DTL from DB200
    int64_t out_ts = s7codec::decodeDtlToNanoseconds(
        s7codec::ReadBuffer{p_arena + p_seg200->offset + 10, 12}, s7codec::DecodeOptions{.endian = s7codec::Endian::Big});
    CHECK(out_ts == sample_dtl_ns);
    CHECK(s7codec::DTL::fromUnixNanoseconds(out_ts).toString() == "2024-03-18 19:30:00.500000000");

    // 7. Verify version bump
    const auto* p_pred_node = state.find("DB200.PredictedSpeed");
    CHECK(p_pred_node != nullptr);
    CHECK(p_pred_node->state_ != nullptr);
    CHECK(p_pred_node->state_->version_.load() > 0);

    // 8. Test unbind
    bridge.unbindState();
    CHECK(!bridge.isBound());
    auto unbind_step = bridge.step(2'000'000'000ULL);
    CHECK(unbind_step.hasError());

    // 9. Test TwinBridge with Little-Endian DataBlocks (DB300 inputs, DB400 outputs)
    {
        state.registerSegment("DB300", 300, 64);
        state.registerSegment("DB400", 400, 64);

        // Input fields with Little-Endian:
        PlcNode node_speed_le;
        node_speed_le.name_ = "Speed";
        node_speed_le.db_number_ = 300;
        node_speed_le.offset_ = 0;
        node_speed_le.type_ = s7codec::Type::Real;
        node_speed_le.endian_ = s7codec::Endian::Little;
        state.add(node_speed_le, "DB300");

        PlcNode node_press_le;
        node_press_le.name_ = "Pressure";
        node_press_le.db_number_ = 300;
        node_press_le.offset_ = 4;
        node_press_le.type_ = s7codec::Type::Real;
        node_press_le.endian_ = s7codec::Endian::Little;
        state.add(node_press_le, "DB300");

        PlcNode node_run_le;
        node_run_le.name_ = "Running";
        node_run_le.db_number_ = 300;
        node_run_le.offset_ = 8;
        node_run_le.bit_index_ = 0;
        node_run_le.type_ = s7codec::Type::Bool;
        node_run_le.endian_ = s7codec::Endian::Little;
        state.add(node_run_le, "DB300");

        PlcNode node_dtl_le;
        node_dtl_le.name_ = "Timestamp";
        node_dtl_le.db_number_ = 300;
        node_dtl_le.offset_ = 10;
        node_dtl_le.type_ = s7codec::Type::DTL;
        node_dtl_le.endian_ = s7codec::Endian::Little;
        state.add(node_dtl_le, "DB300");

        // Output fields with Little-Endian:
        PlcNode node_pred_le;
        node_pred_le.name_ = "PredictedSpeed";
        node_pred_le.db_number_ = 400;
        node_pred_le.offset_ = 0;
        node_pred_le.type_ = s7codec::Type::Real;
        node_pred_le.endian_ = s7codec::Endian::Little;
        state.add(node_pred_le, "DB400");

        PlcNode node_score_le;
        node_score_le.name_ = "AnomalyScore";
        node_score_le.db_number_ = 400;
        node_score_le.offset_ = 4;
        node_score_le.type_ = s7codec::Type::Real;
        node_score_le.endian_ = s7codec::Endian::Little;
        state.add(node_score_le, "DB400");

        PlcNode node_alarm_le;
        node_alarm_le.name_ = "Alarm";
        node_alarm_le.db_number_ = 400;
        node_alarm_le.offset_ = 8;
        node_alarm_le.bit_index_ = 0;
        node_alarm_le.type_ = s7codec::Type::Bool;
        node_alarm_le.endian_ = s7codec::Endian::Little;
        state.add(node_alarm_le, "DB400");

        PlcNode node_out_dtl_le;
        node_out_dtl_le.name_ = "InferenceTimestamp";
        node_out_dtl_le.db_number_ = 400;
        node_out_dtl_le.offset_ = 10;
        node_out_dtl_le.type_ = s7codec::Type::DTL;
        node_out_dtl_le.endian_ = s7codec::Endian::Little;
        state.add(node_out_dtl_le, "DB400");

        state.rebuildFieldIndex();

        auto* p_seg300 = state.findSegmentById(300);
        auto* p_seg400 = state.findSegmentById(400);
        CHECK(p_seg300 != nullptr && p_seg400 != nullptr);

        // Populate DB300 with LE raw values
        s7codec::toLE<float>(1500.0f, p_arena + p_seg300->offset + 0);
        s7codec::toLE<float>(5.0f, p_arena + p_seg300->offset + 4);
        p_arena[p_seg300->offset + 8] = 0x01; // Bool running = true
        s7codec::encodeDtl(sample_dtl_ns, s7codec::WriteBuffer{p_arena + p_seg300->offset + 10, 12}, s7codec::Endian::Little);

        // Configure ModelManifest for DB300 / DB400
        ModelManifest manifest_le;
        manifest_le.name = "twin_test_le_model";
        manifest_le.backend = "simple_predict";

        FeatureDef fle0;
        fle0.name = "speed";
        fle0.source_tag = "DB300.Speed";
        fle0.tag_index = 0;
        fle0.norm = NormType::MinMax;
        fle0.min_val = 0.0;
        fle0.max_val = 3000.0;

        FeatureDef fle1;
        fle1.name = "pressure";
        fle1.source_tag = "DB300.Pressure";
        fle1.tag_index = 1;
        fle1.norm = NormType::MinMax;
        fle1.min_val = 0.0;
        fle1.max_val = 10.0;

        FeatureDef fle2;
        fle2.name = "running";
        fle2.source_tag = "DB300.Running";
        fle2.tag_index = 2;
        fle2.norm = NormType::None;

        FeatureDef fle3;
        fle3.name = "timestamp";
        fle3.source_tag = "DB300.Timestamp";
        fle3.tag_index = 3;
        fle3.norm = NormType::None;

        manifest_le.spec.features = {fle0, fle1, fle2, fle3};
        manifest_le.thresholds.warning = 0.6f;
        manifest_le.thresholds.alarm = 0.8f;

        TwinBridgeConfig bridge_cfg_le;
        bridge_cfg_le.output_bindings = {{"DB400.PredictedSpeed", TwinOutputTarget::PredictionValue, 1},
            {"DB400.AnomalyScore", TwinOutputTarget::AnomalyScore, 0}, {"DB400.Alarm", TwinOutputTarget::AlarmBit, 0},
            {"DB400.InferenceTimestamp", TwinOutputTarget::Timestamp, 0}};
        bridge_cfg_le.write_outputs_to_twin = true;

        TwinBridge bridge_le(bridge_cfg_le);
        CHECK(!bridge_le.loadModel(manifest_le, std::make_unique<SimplePredictBackend>()).hasError());
        CHECK(!bridge_le.bindState(state).hasError());

        auto step_le_res = bridge_le.step(sample_dtl_ns);
        CHECK(!step_le_res.hasError());

        // Verify DB400 outputs in Little-Endian
        float out_speed_le = s7codec::fromLE<float>(p_arena + p_seg400->offset + 0);
        CHECK(std::abs(out_speed_le - 1500.0f) < 1e-3);

        float out_score_le = s7codec::fromLE<float>(p_arena + p_seg400->offset + 4);
        CHECK(out_score_le > 0.0f);

        uint8_t alarm_le_byte = p_arena[p_seg400->offset + 8];
        CHECK((alarm_le_byte & 0x01) == 0);

        int64_t out_ts_le = s7codec::decodeDtlToNanoseconds(
            s7codec::ReadBuffer{p_arena + p_seg400->offset + 10, 12}, s7codec::DecodeOptions{.endian = s7codec::Endian::Little});
        CHECK(out_ts_le == sample_dtl_ns);
        CHECK(s7codec::DTL::fromUnixNanoseconds(out_ts_le).toString() == "2024-03-18 19:30:00.500000000");
    }

    if (g_failures == 0) {
        std::printf("[twin_bridge_test] All tests passed.\n");
        return 0;
    }
    std::printf("[twin_bridge_test] %d tests FAILED.\n", g_failures);
    return 1;
}
