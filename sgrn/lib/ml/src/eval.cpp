#include <sgrn/ml/eval.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace sgrn::ml
{

Result<RegressionMetrics> Evaluator::evaluateRegression(std::span<const float> t_predictions, std::span<const float> t_targets) {
    if (t_predictions.size() != t_targets.size()) {
        return Result<RegressionMetrics>::Error("Predictions and targets size mismatch");
    }
    if (t_predictions.empty()) {
        return Result<RegressionMetrics>::Error("Cannot evaluate regression on empty spans");
    }

    const size_t n = t_predictions.size();
    double sum_target = 0.0;
    for (float t : t_targets) {
        sum_target += t;
    }
    const double mean_target = sum_target / static_cast<double>(n);

    double sum_sq_err = 0.0;
    double sum_abs_err = 0.0;
    double sum_sq_tot = 0.0;
    double max_err = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const double diff = static_cast<double>(t_predictions[i]) - static_cast<double>(t_targets[i]);
        const double abs_diff = std::abs(diff);
        sum_sq_err += diff * diff;
        sum_abs_err += abs_diff;
        if (abs_diff > max_err) {
            max_err = abs_diff;
        }

        const double t_diff = static_cast<double>(t_targets[i]) - mean_target;
        sum_sq_tot += t_diff * t_diff;
    }

    RegressionMetrics m;
    m.sample_count = n;
    m.mse = sum_sq_err / static_cast<double>(n);
    m.rmse = std::sqrt(m.mse);
    m.mae = sum_abs_err / static_cast<double>(n);
    m.max_error = max_err;
    if (sum_sq_tot > 1e-12) {
        m.r2 = 1.0 - (sum_sq_err / sum_sq_tot);
    } else {
        m.r2 = 1.0; // Zero variance in target
    }

    return m;
}

Result<ClassificationMetrics> Evaluator::evaluateBinary(
    std::span<const float> t_predictions, std::span<const float> t_targets, float t_threshold) {
    if (t_predictions.size() != t_targets.size()) {
        return Result<ClassificationMetrics>::Error("Predictions and targets size mismatch");
    }
    if (t_predictions.empty()) {
        return Result<ClassificationMetrics>::Error("Cannot evaluate binary classification on empty spans");
    }

    ClassificationMetrics m;
    m.sample_count = t_predictions.size();

    for (size_t i = 0; i < t_predictions.size(); ++i) {
        const bool pred_pos = t_predictions[i] >= t_threshold;
        const bool true_pos = t_targets[i] >= t_threshold;

        if (pred_pos && true_pos) {
            m.tp++;
        } else if (pred_pos && !true_pos) {
            m.fp++;
        } else if (!pred_pos && !true_pos) {
            m.tn++;
        } else {
            m.fn++;
        }
    }

    const double total = static_cast<double>(m.sample_count);
    m.accuracy = static_cast<double>(m.tp + m.tn) / total;

    const double denom_prec = static_cast<double>(m.tp + m.fp);
    m.precision = denom_prec > 0.0 ? (static_cast<double>(m.tp) / denom_prec) : 0.0;

    const double denom_rec = static_cast<double>(m.tp + m.fn);
    m.recall = denom_rec > 0.0 ? (static_cast<double>(m.tp) / denom_rec) : 0.0;

    const double prec_plus_rec = m.precision + m.recall;
    m.f1 = prec_plus_rec > 0.0 ? (2.0 * m.precision * m.recall / prec_plus_rec) : 0.0;

    return m;
}

Result<TraceComparison> Evaluator::compareSignals(
    std::string_view t_signal_name, std::span<const double> t_sim_trace, std::span<const double> t_real_trace) {
    if (t_sim_trace.size() != t_real_trace.size()) {
        return Result<TraceComparison>::Error("Simulated and real trace lengths do not match");
    }
    if (t_sim_trace.empty()) {
        return Result<TraceComparison>::Error("Cannot compare empty traces");
    }

    const size_t n = t_sim_trace.size();
    double sum_sim = 0.0;
    double sum_real = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum_sim += t_sim_trace[i];
        sum_real += t_real_trace[i];
    }
    const double mean_sim = sum_sim / static_cast<double>(n);
    const double mean_real = sum_real / static_cast<double>(n);

    double sum_sq_err = 0.0;
    double sum_abs_err = 0.0;
    double max_dev = 0.0;
    size_t max_dev_idx = 0;

    double num_corr = 0.0;
    double den_sim = 0.0;
    double den_real = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const double diff = t_sim_trace[i] - t_real_trace[i];
        const double abs_diff = std::abs(diff);
        sum_sq_err += diff * diff;
        sum_abs_err += abs_diff;
        if (abs_diff > max_dev) {
            max_dev = abs_diff;
            max_dev_idx = i;
        }

        const double d_sim = t_sim_trace[i] - mean_sim;
        const double d_real = t_real_trace[i] - mean_real;
        num_corr += d_sim * d_real;
        den_sim += d_sim * d_sim;
        den_real += d_real * d_real;
    }

    TraceComparison cmp;
    cmp.signal_name = std::string(t_signal_name);
    cmp.length = n;
    cmp.rmse = std::sqrt(sum_sq_err / static_cast<double>(n));
    cmp.mae = sum_abs_err / static_cast<double>(n);
    cmp.max_abs_dev = max_dev;
    cmp.max_dev_index = max_dev_idx;

    const double den = std::sqrt(den_sim * den_real);
    if (den > 1e-12) {
        cmp.pearson_correlation = num_corr / den;
    } else {
        cmp.pearson_correlation = 1.0;
    }

    return cmp;
}

} // namespace sgrn::ml
