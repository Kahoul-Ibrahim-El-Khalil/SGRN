#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/DbSchema.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sgrn::ml
{

/**
 * @brief Accumulator for a single discrete time-grid bucket cell.
 */
struct ResamplerCell {
    double sum{0.0};
    double max_val{-std::numeric_limits<double>::infinity()};
    double min_val{std::numeric_limits<double>::infinity()};
    double last_val{0.0};
    uint32_t count{0};
    bool bool_or{false};

    void addSample(double t_val, bool t_is_bool) noexcept {
        sum += t_val;
        if (t_val > max_val)
            max_val = t_val;
        if (t_val < min_val)
            min_val = t_val;
        last_val = t_val;
        if (t_is_bool && t_val != 0.0)
            bool_or = true;
        count++;
    }

    void reset() noexcept {
        sum = 0.0;
        max_val = -std::numeric_limits<double>::infinity();
        min_val = std::numeric_limits<double>::infinity();
        last_val = 0.0;
        count = 0;
        bool_or = false;
    }

    [[nodiscard]] float finish(sgrn::scl::AggregationType t_agg, bool t_is_bool) const noexcept {
        if (count == 0)
            return 0.0f;
        if (t_is_bool) {
            if (t_agg == sgrn::scl::AggregationType::Last) {
                return last_val != 0.0 ? 1.0f : 0.0f;
            }
            return bool_or ? 1.0f : 0.0f;
        }

        switch (t_agg) {
            case sgrn::scl::AggregationType::Max:
                return static_cast<float>(max_val);
            case sgrn::scl::AggregationType::Min:
                return static_cast<float>(min_val);
            case sgrn::scl::AggregationType::Last:
                return static_cast<float>(last_val);
            case sgrn::scl::AggregationType::Or:
                return bool_or ? 1.0f : 0.0f;
            case sgrn::scl::AggregationType::Mean:
            case sgrn::scl::AggregationType::Default:
            default:
                return static_cast<float>(sum / count);
        }
    }
};

/**
 * @brief Metadata configuration for one feature column in the discrete grid.
 */
struct ResamplerFieldConfig {
    std::string name;                                                         ///< Full tag path, e.g. "DB10.Speed"
    uint16_t db_num{0};                                                       ///< DB number
    size_t offset{0};                                                         ///< Byte offset in DB
    uint8_t bit_index{0};                                                     ///< Bit offset if boolean
    s7codec::Type type{s7codec::Type::Real};                                  ///< DataType
    sgrn::scl::AggregationType agg_type{sgrn::scl::AggregationType::Default}; ///< Aggregation rule
    std::string clock_tag;                                                    ///< Associated DTL clock tag path
    bool is_label{false};                                                     ///< Flagged as target label
    bool is_boolean{false};                                                   ///< Cached bool check
};

/**
 * @brief Configuration parameters for TimeGridResampler.
 */
struct ResamplerConfig {
    uint64_t step_ns{10'000'000ULL};          ///< Target discrete grid step Δt (default: 10ms)
    std::string primary_clock_tag;            ///< Default master clock tag
    bool forward_fill{true};                  ///< Carry forward previous valid value for empty buckets
    std::vector<ResamplerFieldConfig> fields; ///< Ordered columns in the output grid
};

/**
 * @brief Represents a single synchronized row on the discrete time grid.
 */
struct DiscreteGridRow {
    uint64_t timestamp_ns{0};  ///< Exact uniform grid time t_k = t_0 + k · Δt
    std::vector<float> values; ///< Sliced values aligned to ResamplerConfig::fields
    std::vector<bool> valid;   ///< True if row had real updates or was forward-filled
};

/**
 * @brief Zero-jitter, discrete time-grid resampling engine.
 *
 * Synchronizes multi-rate, asynchronous, or event-driven PLC DataBlock telemetry
 * into a strictly uniform discrete time grid t_k = t_0 + k · Δt.
 *
 * Applies typed aggregation rules (Mean, Max, Min, OR, Last) and zero-order hold
 * forward-fill across communication gaps.
 */
class TimeGridResampler {
public:
    using RowCallback = std::function<void(const DiscreteGridRow& t_row)>;

    TimeGridResampler() = default;
    explicit TimeGridResampler(ResamplerConfig t_config);

    void setConfig(ResamplerConfig t_config);
    [[nodiscard]] const ResamplerConfig& config() const noexcept {
        return config_;
    }

    void setRowCallback(RowCallback t_callback) {
        row_callback_ = std::move(t_callback);
    }

    /// Resets resampler state (accumulators, forward-fill cache, grid origin)
    void reset();

    /// Ingest a single numerical sample for a specific field index
    void pushSample(size_t t_field_idx, double t_value, uint64_t t_sample_clock_ns);

    /// Ingest a single numerical sample by full field tag name
    void pushNamedSample(std::string_view t_tag, double t_value, uint64_t t_sample_clock_ns);

    /// Flush the current active bucket to the callback
    void flush();

    [[nodiscard]] uint64_t totalEmittedRows() const noexcept {
        return total_emitted_rows_;
    }

    [[nodiscard]] uint64_t stepNs() const noexcept {
        return config_.step_ns;
    }

    [[nodiscard]] uint64_t startTsNs() const noexcept {
        return start_ts_ns_;
    }

    [[nodiscard]] bool hasStarted() const noexcept {
        return has_started_;
    }

private:
    void closeAndEmitBucket(uint64_t t_bucket_idx);
    void emitForwardFilledBucket(uint64_t t_gap_idx);
    void resetAccumulators();
    [[nodiscard]] bool isBool(size_t t_field_idx) const noexcept;

    ResamplerConfig config_;
    RowCallback row_callback_;

    std::unordered_map<std::string, size_t> field_index_map_;
    std::vector<ResamplerCell> accumulators_;
    std::vector<float> last_valid_values_;
    std::vector<bool> has_last_valid_;

    uint64_t start_ts_ns_{0};
    uint64_t current_bucket_idx_{0};
    uint64_t total_emitted_rows_{0};
    bool has_started_{false};
};

/**
 * @brief Factory helper constructing a ResamplerConfig from SCL schemas.
 */
ResamplerConfig makeResamplerConfig(
    const std::vector<sgrn::scl::DbSchema>& t_schemas, uint64_t t_step_override_ns = 0, std::string_view t_clock_tag_override = "");

ResamplerConfig makeResamplerConfig(
    const sgrn::scl::DbSchema& t_schema, uint64_t t_step_override_ns = 0, std::string_view t_clock_tag_override = "");

} // namespace sgrn::ml
