#pragma once

#include <sgrn/Result.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sgrn::ml
{

struct RegressionMetrics {
    double mse = 0.0;
    double rmse = 0.0;
    double mae = 0.0;
    double r2 = 0.0;
    double max_error = 0.0;
    size_t sample_count = 0;
};

struct ClassificationMetrics {
    double accuracy = 0.0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    uint64_t tp = 0;
    uint64_t fp = 0;
    uint64_t tn = 0;
    uint64_t fn = 0;
    size_t sample_count = 0;
};

struct TraceComparison {
    std::string signal_name;
    double rmse = 0.0;
    double mae = 0.0;
    double max_abs_dev = 0.0;
    size_t max_dev_index = 0;
    double pearson_correlation = 0.0;
    size_t length = 0;
};

class Evaluator {
public:
    static Result<RegressionMetrics> evaluateRegression(std::span<const float> t_predictions, std::span<const float> t_targets);

    static Result<ClassificationMetrics> evaluateBinary(
        std::span<const float> t_predictions, std::span<const float> t_targets, float t_threshold = 0.5f);

    static Result<TraceComparison> compareSignals(
        std::string_view t_signal_name, std::span<const double> t_sim_trace, std::span<const double> t_real_trace);
};

} // namespace sgrn::ml
