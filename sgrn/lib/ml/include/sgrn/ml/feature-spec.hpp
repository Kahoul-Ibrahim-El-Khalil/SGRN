#pragma once

#include <sgrn/Result.hpp>
#include <cstdint>
#include <rapidjson/document.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::ml
{

enum class NormType : uint8_t { None = 0, MinMax = 1, ZScore = 2 };

enum class DerivedType : uint8_t {
    Raw = 0,
    RateOfChange = 1,  // (x_curr - x_prev) / dt_sec
    Delta = 2,         // x_curr - x_prev
    MovingAverage = 3, // SMA over window N
    Lag = 4            // value from N steps back
};

struct FeatureDef {
    std::string name;
    std::string source_tag;
    uint32_t tag_index = 0; // Index in raw inputs span
    DerivedType derived = DerivedType::Raw;
    double param = 0.0; // Window size for SMA, lag count for Lag, etc.
    NormType norm = NormType::None;
    double min_val = 0.0;
    double max_val = 1.0;
    double mean_val = 0.0;
    double std_val = 1.0;

    [[nodiscard]] float normalize(float t_val) const noexcept;
};

enum class TaskType : uint8_t { Regression = 0, BinaryClassification = 1, MultiClassification = 2 };

struct LabelDef {
    std::string name;
    std::string target_tag;
    TaskType task = TaskType::Regression;
    double threshold = 0.5;
    std::vector<std::string> classes;
};

class FeatureSpec {
public:
    FeatureSpec() = default;

    std::string name;
    std::string version = "1.0.0";
    std::vector<FeatureDef> features;
    std::vector<LabelDef> labels;

    [[nodiscard]] size_t inputSize() const noexcept {
        return features.size();
    }

    [[nodiscard]] size_t outputSize() const noexcept {
        return labels.size();
    }

    [[nodiscard]] int findFeatureIndex(std::string_view t_name) const noexcept;
    [[nodiscard]] int findLabelIndex(std::string_view t_name) const noexcept;

    [[nodiscard]] std::string toJson(bool t_pretty = false) const;
    static Result<FeatureSpec> fromJson(std::string_view t_json_str);
    static Result<FeatureSpec> fromJsonValue(const rapidjson::Value& t_val);
    void toJsonValue(rapidjson::Value& t_out, rapidjson::Document::AllocatorType& t_allocator) const;
};

} // namespace sgrn::ml
