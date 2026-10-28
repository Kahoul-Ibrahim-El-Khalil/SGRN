#include <sgrn/Result.hpp>
#include <sgrn/ml/backend.hpp>
#include <sgrn/ml/engine.hpp>
#include <sgrn/ml/eval.hpp>
#include <sgrn/ml/manifest.hpp>
#include <sgrn/utils/strings.hpp>

#ifdef SGRN_HAS_TORCH
#include <sgrn/ml_torch/backend.hpp>
#endif

#include <fmt/color.h>
#include <fmt/core.h>
#include <cxxopts.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct LoadedCsv {
    std::vector<std::string> headers;
    std::vector<std::vector<double>> rows;

    [[nodiscard]] int findCol(std::string_view t_name) const {
        for (size_t i = 0; i < headers.size(); ++i) {
            if (headers[i] == t_name)
                return static_cast<int>(i);
        }
        return -1;
    }
};

static sgrn::Result<LoadedCsv> loadCsv(const std::filesystem::path& t_path) {
    std::ifstream file(t_path);
    if (!file.is_open()) {
        return std::string("Cannot open CSV file: " + t_path.string());
    }

    std::string line;
    if (!std::getline(file, line)) {
        return std::string("CSV file is empty: " + t_path.string());
    }

    LoadedCsv csv;
    csv.headers = sgrn::utils::strings::split(line, ',');
    for (auto& h : csv.headers) {
        h = sgrn::utils::strings::trim(h);
    }

    while (std::getline(file, line)) {
        if (line.empty())
            continue;
        auto tokens = sgrn::utils::strings::split(line, ',');
        if (tokens.size() != csv.headers.size())
            continue;

        std::vector<double> row;
        row.reserve(tokens.size());
        bool valid = true;
        for (const auto& tok : tokens) {
            std::string t = sgrn::utils::strings::trim(tok);
            if (t.empty()) {
                valid = false;
                break;
            }
            try {
                row.push_back(std::stod(t));
            } catch (...) {
                valid = false;
                break;
            }
        }
        if (valid) {
            csv.rows.push_back(std::move(row));
        }
    }

    if (csv.rows.empty()) {
        return sgrn::Result<LoadedCsv>::Error("No valid rows found in CSV: " + t_path.string());
    }

    return csv;
}

static int runModelEvaluation(const std::string& t_manifest_path, const std::string& t_data_path, const std::string& t_weights_override) {
    fmt::print(fmt::fg(fmt::color::cyan), "=== Model Evaluation Mode ===\n");
    fmt::print("Loading manifest: {}\n", t_manifest_path);

    auto m_res = sgrn::ml::ModelManifest::fromFile(t_manifest_path);
    if (m_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading manifest: {}\n", m_res.error());
        return 1;
    }

    auto manifest = std::move(m_res.value());
    std::string weights_path = t_weights_override.empty() ? manifest.weights_file : t_weights_override;
    fmt::print("Backend: {}, Weights: {}\n", manifest.backend, weights_path);

    // Verify weights integrity if checksum is present
    if (!weights_path.empty()) {
        auto check_res = manifest.validateWeightsFile(weights_path);
        if (check_res.hasError()) {
            fmt::print(stderr, fmt::fg(fmt::color::yellow), "Warning: Weights validation: {}\n", check_res.error());
        } else if (!manifest.weights_sha256.empty()) {
            fmt::print(fmt::fg(fmt::color::green), "Weights SHA256 integrity check verified.\n");
        }
    }

#ifdef SGRN_HAS_TORCH
    if (manifest.backend == "torch") {
        sgrn::ml_torch::TorchBackend::registerWithFactory();
    }
#endif

    auto backend_res = sgrn::ml::BackendFactory::createBackend(manifest.backend);
    if (backend_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error creating backend: {}\n", backend_res.error());
        return 1;
    }

    auto backend = std::move(backend_res.value());
    auto load_res = backend->load(weights_path, manifest);
    if (load_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading weights: {}\n", load_res.error());
        return 1;
    }

    sgrn::ml::PredictionEngine engine;
    auto eng_res = engine.loadModel(manifest, std::move(backend));
    if (eng_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading engine: {}\n", eng_res.error());
        return 1;
    }

    fmt::print("Loading test dataset: {}\n", t_data_path);
    auto csv_res = loadCsv(t_data_path);
    if (csv_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading test CSV: {}\n", csv_res.error());
        return 1;
    }

    const auto& csv = csv_res.value();
    fmt::print("Evaluating across {} test samples...\n", csv.rows.size());

    std::vector<float> all_predictions;
    std::vector<float> all_targets;
    all_predictions.reserve(csv.rows.size());
    all_targets.reserve(csv.rows.size());

    int target_col_idx = -1;
    if (!manifest.spec.labels.empty()) {
        target_col_idx = csv.findCol(manifest.spec.labels[0].target_tag);
    }
    if (target_col_idx < 0) {
        target_col_idx = static_cast<int>(csv.headers.size() - 1);
    }

    uint64_t dummy_ts = 1'000'000'000ULL;
    for (const auto& row : csv.rows) {
        auto pred_res = engine.step(dummy_ts += 100'000'000ULL, row);
        if (pred_res.hasError()) {
            fmt::print(stderr, fmt::fg(fmt::color::red), "Inference error: {}\n", pred_res.error());
            return 1;
        }

        const auto& pred = pred_res.value();
        if (!pred.outputs.empty()) {
            all_predictions.push_back(pred.outputs[0]);
            all_targets.push_back(static_cast<float>(row[target_col_idx]));
        }
    }

    // Evaluate regression metrics
    auto reg_res = sgrn::ml::Evaluator::evaluateRegression(all_predictions, all_targets);
    if (!reg_res.hasError()) {
        const auto& m = reg_res.value();
        fmt::print(fmt::fg(fmt::color::green), "--- Evaluation Metrics ---\n");
        fmt::print("  Sample Count : {}\n", m.sample_count);
        fmt::print("  MSE          : {:.6f}\n", m.mse);
        fmt::print("  RMSE         : {:.6f}\n", m.rmse);
        fmt::print("  MAE          : {:.6f}\n", m.mae);
        fmt::print("  Max Error    : {:.6f}\n", m.max_error);
        fmt::print("  R^2 Score    : {:.6f}\n", m.r2);
    }

    return 0;
}

static int runTraceComparison(const std::string& t_sim_path, const std::string& t_real_path, const std::string& t_signal_filter) {
    fmt::print(fmt::fg(fmt::color::cyan), "=== Simulated vs Real Trace Comparison Mode ===\n");
    fmt::print("Simulated CSV: {}\n", t_sim_path);
    fmt::print("Real CSV     : {}\n", t_real_path);

    auto sim_res = loadCsv(t_sim_path);
    if (sim_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading sim CSV: {}\n", sim_res.error());
        return 1;
    }
    auto real_res = loadCsv(t_real_path);
    if (real_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading real CSV: {}\n", real_res.error());
        return 1;
    }

    const auto& sim_csv = sim_res.value();
    const auto& real_csv = real_res.value();

    const size_t n_rows = std::min(sim_csv.rows.size(), real_csv.rows.size());
    if (n_rows == 0) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Zero common rows to compare\n");
        return 1;
    }

    fmt::print("Comparing {} time points...\n\n", n_rows);
    fmt::print("{:<30} {:<12} {:<12} {:<12} {:<12}\n", "Signal", "RMSE", "MAE", "Max Dev", "Pearson r");
    fmt::print("{:-<30} {:-<12} {:-<12} {:-<12} {:-<12}\n", "", "", "", "", "");

    for (const auto& header : sim_csv.headers) {
        if (!t_signal_filter.empty() && header != t_signal_filter)
            continue;
        int r_idx = real_csv.findCol(header);
        int s_idx = sim_csv.findCol(header);
        if (r_idx < 0 || s_idx < 0)
            continue;

        std::vector<double> sim_trace(n_rows);
        std::vector<double> real_trace(n_rows);
        for (size_t i = 0; i < n_rows; ++i) {
            sim_trace[i] = sim_csv.rows[i][s_idx];
            real_trace[i] = real_csv.rows[i][r_idx];
        }

        auto cmp_res = sgrn::ml::Evaluator::compareSignals(header, sim_trace, real_trace);
        if (cmp_res.hasError())
            continue;

        const auto& c = cmp_res.value();
        fmt::print("{:<30} {:<12.4f} {:<12.4f} {:<12.4f} {:<12.4f}\n", c.signal_name, c.rmse, c.mae, c.max_abs_dev, c.pearson_correlation);
    }

    return 0;
}

int main(int t_argc, char** tp_argv) {
    cxxopts::Options options("sgrn_ml_eval", "SGRN Model Evaluation & Trace Comparator CLI");

    options.add_options()("m,manifest", "Path to ModelManifest JSON file", cxxopts::value<std::string>())(
        "d,data", "Path to test dataset CSV file", cxxopts::value<std::string>())("w,weights", "Override model weights path",
        cxxopts::value<std::string>())("sim", "Path to simulated telemetry CSV for trace comparison", cxxopts::value<std::string>())(
        "real", "Path to real/baseline telemetry CSV for trace comparison", cxxopts::value<std::string>())(
        "signal", "Signal name filter for trace comparison", cxxopts::value<std::string>())("h,help", "Print usage help");

    auto result = options.parse(t_argc, tp_argv);

    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        return 0;
    }

    if (result.count("sim") && result.count("real")) {
        std::string filter = result.count("signal") ? result["signal"].as<std::string>() : "";
        return runTraceComparison(result["sim"].as<std::string>(), result["real"].as<std::string>(), filter);
    }

    if (result.count("manifest") && result.count("data")) {
        std::string weights = result.count("weights") ? result["weights"].as<std::string>() : "";
        return runModelEvaluation(result["manifest"].as<std::string>(), result["data"].as<std::string>(), weights);
    }

    fmt::print(
        stderr, "Error: Specify either (--manifest and --data) for model evaluation, or (--sim and --real) for trace comparison.\n\n");
    fmt::print("{}\n", options.help());
    return 1;
}
