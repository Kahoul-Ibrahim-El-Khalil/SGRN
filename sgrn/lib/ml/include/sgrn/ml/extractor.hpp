#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/feature_spec.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace sgrn::ml
{

class FeatureExtractor {
public:
    FeatureExtractor() = default;
    explicit FeatureExtractor(FeatureSpec t_spec);

    Result<void> setSpec(FeatureSpec t_spec);
    [[nodiscard]] const FeatureSpec& spec() const noexcept {
        return spec_;
    }

    [[nodiscard]] size_t inputSize() const noexcept {
        return spec_.features.size();
    }

    /// Extract features from raw inputs into pre-allocated out_features span.
    /// Returns Error if raw_inputs or out_features size does not match spec requirements.
    Result<void> push(uint64_t t_ts_ns, std::span<const double> t_raw_inputs, std::span<float> t_out_features);

    /// Convenience wrapper that allocates and returns a vector.
    Result<std::vector<float>> extract(uint64_t t_ts_ns, std::span<const double> t_raw_inputs);

    void reset();

private:
    struct FeatureState {
        uint64_t last_ts_ns = 0;
        double last_value = 0.0;
        bool has_last = false;
        std::deque<double> window;
    };

    FeatureSpec spec_;
    std::vector<FeatureState> states_;
};

} // namespace sgrn::ml
