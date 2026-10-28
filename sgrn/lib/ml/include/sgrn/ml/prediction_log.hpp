#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/engine.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace sgrn::ml
{

#pragma pack(push, 1)
struct PredictionRecordHeader {
    uint64_t timestamp_ns = 0;
    uint32_t model_id = 0;
    uint32_t latency_us = 0;
    uint8_t verdict_status = 0;
    uint8_t flags = 0;
    uint16_t num_outputs = 0;
    float anomaly_score = 0.0f;
    float confidence = 1.0f;
};
#pragma pack(pop)

static_assert(sizeof(PredictionRecordHeader) == 28, "PredictionRecordHeader must be exactly 28 bytes packed");

struct PredictionRecord {
    PredictionRecordHeader header;
    std::vector<float> outputs;

    [[nodiscard]] size_t serializedSize() const noexcept {
        return sizeof(PredictionRecordHeader) + outputs.size() * sizeof(float);
    }

    void serialize(std::vector<uint8_t>& buf) const {
        const size_t start = buf.size();
        buf.resize(start + serializedSize());
        auto* ptr = buf.data() + start;

        PredictionRecordHeader h = header;
        h.num_outputs = static_cast<uint16_t>(outputs.size());
        std::memcpy(ptr, &h, sizeof(h));

        if (!outputs.empty()) {
            std::memcpy(ptr + sizeof(h), outputs.data(), outputs.size() * sizeof(float));
        }
    }

    static Result<PredictionRecord> deserialize(std::span<const uint8_t> data) {
        if (data.size() < sizeof(PredictionRecordHeader)) {
            return Result<PredictionRecord>::Error("Buffer too small for PredictionRecordHeader");
        }

        PredictionRecord rec;
        std::memcpy(&rec.header, data.data(), sizeof(PredictionRecordHeader));

        const size_t expected_total = sizeof(PredictionRecordHeader) + rec.header.num_outputs * sizeof(float);
        if (data.size() < expected_total) {
            return Result<PredictionRecord>::Error("Buffer truncated for PredictionRecord payload");
        }

        rec.outputs.resize(rec.header.num_outputs);
        if (rec.header.num_outputs > 0) {
            std::memcpy(rec.outputs.data(), data.data() + sizeof(PredictionRecordHeader), rec.header.num_outputs * sizeof(float));
        }

        return rec;
    }

    static PredictionRecord fromPrediction(const Prediction& pred, uint32_t model_id = 0) {
        PredictionRecord rec;
        rec.header.timestamp_ns = pred.timestamp_ns;
        rec.header.model_id = model_id;
        rec.header.latency_us = pred.latency_us;
        rec.header.verdict_status = static_cast<uint8_t>(pred.verdict.status);
        rec.header.flags = 0;
        rec.header.anomaly_score = pred.verdict.anomaly_score;
        rec.header.confidence = pred.verdict.confidence;
        rec.header.num_outputs = static_cast<uint16_t>(pred.outputs.size());
        rec.outputs = pred.outputs;
        return rec;
    }
};

} // namespace sgrn::ml
