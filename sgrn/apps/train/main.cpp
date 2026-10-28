#include <sgrn/Result.hpp>
#include <sgrn/ml/columnar_dataset.hpp>
#include <sgrn/ml/feature_spec.hpp>
#include <sgrn/ml/manifest.hpp>
#include <sgrn/ml_torch/mlp.hpp>
#include <sgrn/ml_torch/trainer.hpp>
#include <sgrn/utils/strings.hpp>

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
};

static sgrn::Result<LoadedCsv> loadCsv(const std::filesystem::path& t_path) {
    std::ifstream file(t_path);
    if (!file.is_open()) {
        return sgrn::Result<LoadedCsv>::Error("Cannot open CSV file: " + t_path.string());
    }

    std::string line;
    if (!std::getline(file, line)) {
        return sgrn::Result<LoadedCsv>::Error("CSV file is empty: " + t_path.string());
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
        if (tokens.size() != csv.headers.size()) {
            continue; // Skip malformed rows
        }

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
        return sgrn::Result<LoadedCsv>::Error("No valid numerical rows loaded from CSV");
    }

    return csv;
}

int main(int t_argc, char** tp_argv) {
    cxxopts::Options options("sgrn_train", "SGRN In-Tree PyTorch ML Model Training CLI");

    options.add_options()("d,data", "Path to training dataset CSV file", cxxopts::value<std::string>())("t,target",
        "Target column name in CSV",
        cxxopts::value<std::string>())("s,spec", "Path to custom FeatureSpec JSON file (optional)", cxxopts::value<std::string>())(
        "o,output-dir", "Output directory for trained weights and manifest", cxxopts::value<std::string>()->default_value("./models"))(
        "n,name", "Model name", cxxopts::value<std::string>()->default_value("sgrn_model"))("e,epochs", "Number of training epochs",
        cxxopts::value<size_t>()->default_value("20"))("b,batch-size", "Mini-batch size", cxxopts::value<size_t>()->default_value("32"))(
        "lr", "Learning rate", cxxopts::value<double>()->default_value("0.001"))(
        "weight-decay", "Weight decay (L2 penalty)", cxxopts::value<double>()->default_value("0.0001"))(
        "optimizer", "Optimizer ('adamw', 'adam', 'sgd')", cxxopts::value<std::string>()->default_value("adamw"))(
        "loss", "Loss function ('mse', 'bce', 'l1')", cxxopts::value<std::string>()->default_value("mse"))(
        "hidden", "Comma-separated hidden layer dimensions", cxxopts::value<std::string>()->default_value("64,32"))(
        "activation", "Activation function ('relu', 'gelu', 'tanh', 'sigmoid')", cxxopts::value<std::string>()->default_value("relu"))(
        "val-split", "Validation split ratio (0.0 to 0.5)", cxxopts::value<float>()->default_value("0.2"))("device",
        "Target device ('auto', 'cpu', 'cuda')", cxxopts::value<std::string>()->default_value("auto"))("h,help", "Print usage help");

    options.parse_positional({"data"});

    auto result = options.parse(t_argc, tp_argv);

    if (result.count("help") || !result.count("data")) {
        fmt::print("{}\n", options.help());
        return result.count("help") ? 0 : 1;
    }

    const std::string data_path = result["data"].as<std::string>();
    const std::string model_name = result["name"].as<std::string>();
    const std::string out_dir = result["output-dir"].as<std::string>();

    fmt::print(fmt::fg(fmt::color::cyan), "=========================================================\n");
    fmt::print(fmt::fg(fmt::color::cyan), " SGRN PyTorch In-Tree Model Trainer\n");
    fmt::print(fmt::fg(fmt::color::cyan), "=========================================================\n");
    fmt::print("Loading dataset: {}\n", data_path);

    // ---------------------------------------------------------------
    // Branching point: .sgrnb (zero-copy columnar) vs .csv (legacy)
    // ---------------------------------------------------------------
    const bool is_sgrnb = data_path.size() > 6 && data_path.substr(data_path.size() - 6) == ".sgrnb";

    torch::Tensor x_tensor;
    torch::Tensor y_tensor;
    sgrn::ml::FeatureSpec spec;
    int target_idx = -1; // Only used for CSV path
    std::string target_col;

    if (is_sgrnb) {
        // ---- .sgrnb zero-copy fast path ----
        auto reader_res = sgrn::ml::ColumnarDatasetReader::open(data_path);
        if (reader_res.hasError()) {
            fmt::print(stderr, fmt::fg(fmt::color::red), "Error: {}\n", reader_res.error());
            return 1;
        }
        auto& reader = *reader_res.value();

        const size_t n_rows = reader.rowCount();
        const size_t n_feat = reader.featureCount();
        const size_t n_lbl = reader.labelCount();
        fmt::print("Loaded .sgrnb: {} rows, {} features, {} labels (step={}ns)\n", n_rows, n_feat, n_lbl, reader.stepNs());

        if (n_rows == 0 || n_feat == 0) {
            fmt::print(stderr, fmt::fg(fmt::color::red), "Error: empty .sgrnb dataset\n");
            return 1;
        }

        // Build X tensor: [n_rows × n_feat] from contiguous feature columns
        std::vector<float> x_data(n_rows * n_feat);
        for (size_t c = 0; c < n_feat; ++c) {
            auto col = reader.featureColumn(c);
            for (size_t r = 0; r < n_rows; ++r) {
                x_data[r * n_feat + c] = col[r];
            }
        }
        x_tensor = torch::from_blob(x_data.data(), {static_cast<int64_t>(n_rows), static_cast<int64_t>(n_feat)}, torch::kFloat32).clone();

        // Build Y tensor: [n_rows × n_lbl] from label columns
        const size_t out_dim = std::max(n_lbl, static_cast<size_t>(1));
        std::vector<float> y_data(n_rows * out_dim, 0.0f);
        for (size_t c = 0; c < n_lbl; ++c) {
            auto col = reader.labelColumn(c);
            for (size_t r = 0; r < n_rows; ++r) {
                y_data[r * out_dim + c] = col[r];
            }
        }
        y_tensor = torch::from_blob(y_data.data(), {static_cast<int64_t>(n_rows), static_cast<int64_t>(out_dim)}, torch::kFloat32).clone();

        // Inherit spec from the embedded manifest (or build one)
        const auto& bin_manifest = reader.manifest();
        if (!bin_manifest.spec.features.empty()) {
            spec = bin_manifest.spec;
        } else {
            spec.name = model_name + "_spec";
            for (size_t i = 0; i < n_feat; ++i) {
                sgrn::ml::FeatureDef f;
                f.name = fmt::format("feature_{}", i);
                f.source_tag = f.name;
                f.tag_index = static_cast<uint32_t>(i);
                f.derived = sgrn::ml::DerivedType::Raw;
                f.norm = sgrn::ml::NormType::None;
                spec.features.push_back(std::move(f));
            }
        }
        // If there's a user-specified target, resolve it for the spec
        if (result.count("target")) {
            target_col = result["target"].as<std::string>();
        } else if (!spec.labels.empty()) {
            target_col = spec.labels.front().name;
        }

        // Ensure at least one label exists for MLP output layer
        if (spec.labels.empty()) {
            sgrn::ml::LabelDef l;
            l.name = target_col.empty() ? "target" : target_col;
            l.target_tag = l.name;
            l.task = sgrn::ml::TaskType::Regression;
            spec.labels.push_back(std::move(l));
        }

    } else {
        // ---- Legacy CSV path ----
        auto csv_res = loadCsv(data_path);
        if (csv_res.hasError()) {
            fmt::print(stderr, fmt::fg(fmt::color::red), "Error: {}\n", csv_res.error());
            return 1;
        }

        const auto& csv = csv_res.value();
        fmt::print("Loaded {} rows, {} columns.\n", csv.rows.size(), csv.headers.size());

        // Resolve target column
        if (result.count("target")) {
            target_col = result["target"].as<std::string>();
        } else {
            target_col = csv.headers.back();
        }

        for (size_t i = 0; i < csv.headers.size(); ++i) {
            if (csv.headers[i] == target_col) {
                target_idx = static_cast<int>(i);
                break;
            }
        }

        if (target_idx < 0) {
            fmt::print(stderr, fmt::fg(fmt::color::red), "Target column '{}' not found in CSV headers\n", target_col);
            return 1;
        }

        // Build or load FeatureSpec
        if (result.count("spec")) {
            auto spec_res = sgrn::ml::FeatureSpec::fromJson(result["spec"].as<std::string>());
            if (spec_res.hasError()) {
                fmt::print(stderr, fmt::fg(fmt::color::red), "Error loading spec: {}\n", spec_res.error());
                return 1;
            }
            spec = std::move(spec_res.value());
        } else {
            spec.name = model_name + "_spec";
            for (size_t i = 0; i < csv.headers.size(); ++i) {
                if (static_cast<int>(i) == target_idx)
                    continue;
                sgrn::ml::FeatureDef f;
                f.name = csv.headers[i];
                f.source_tag = csv.headers[i];
                f.tag_index = static_cast<uint32_t>(i);
                f.derived = sgrn::ml::DerivedType::Raw;
                f.norm = sgrn::ml::NormType::MinMax;

                // Compute min / max
                double min_v = csv.rows[0][i];
                double max_v = csv.rows[0][i];
                for (const auto& r : csv.rows) {
                    min_v = std::min(min_v, r[i]);
                    max_v = std::max(max_v, r[i]);
                }
                f.min_val = min_v;
                f.max_val = (max_v > min_v) ? max_v : (min_v + 1.0);
                spec.features.push_back(std::move(f));
            }

            sgrn::ml::LabelDef l;
            l.name = target_col;
            l.target_tag = target_col;
            l.task = sgrn::ml::TaskType::Regression;
            spec.labels.push_back(std::move(l));
        }

        // Prepare Tensors
        const size_t n_samples = csv.rows.size();
        const size_t n_features = spec.features.size();
        std::vector<float> x_data(n_samples * n_features);
        std::vector<float> y_data(n_samples);

        for (size_t r = 0; r < n_samples; ++r) {
            for (size_t f = 0; f < n_features; ++f) {
                const auto& feat_def = spec.features[f];
                const float raw = static_cast<float>(csv.rows[r][feat_def.tag_index]);
                x_data[r * n_features + f] = feat_def.normalize(raw);
            }
            y_data[r] = static_cast<float>(csv.rows[r][target_idx]);
        }

        x_tensor =
            torch::from_blob(x_data.data(), {static_cast<int64_t>(n_samples), static_cast<int64_t>(n_features)}, torch::kFloat32).clone();
        y_tensor = torch::from_blob(y_data.data(), {static_cast<int64_t>(n_samples), 1}, torch::kFloat32).clone();
    }

    const size_t n_features = static_cast<size_t>(x_tensor.size(1));

    fmt::print("Configured {} input features, {} target labels.\n", spec.inputSize(), spec.outputSize());

    // Configure MLP
    sgrn::ml::MlpConfig mlp_cfg;
    const std::string hidden_str = result["hidden"].as<std::string>();
    const std::string activation = result["activation"].as<std::string>();
    auto hidden_tokens = sgrn::utils::strings::split(hidden_str, ',');
    for (const auto& h_tok : hidden_tokens) {
        std::string trimmed = sgrn::utils::strings::trim(h_tok);
        if (!trimmed.empty()) {
            mlp_cfg.layers.push_back({static_cast<size_t>(std::stoul(trimmed)), activation, 0.0f});
        }
    }

    sgrn::ml_torch::Mlp model(n_features, mlp_cfg, spec.outputSize());

    // Configure Device
    torch::Device target_device(torch::kCPU);
    const std::string dev_arg = result["device"].as<std::string>();
    if (dev_arg == "cuda" || (dev_arg == "auto" && torch::cuda::is_available())) {
        target_device = torch::Device(torch::kCUDA);
        fmt::print("Using device: CUDA/ROCm GPU\n");
    } else {
        fmt::print("Using device: CPU\n");
    }

    // Configure Trainer
    sgrn::ml_torch::TrainerConfig train_cfg;
    train_cfg.epochs = result["epochs"].as<size_t>();
    train_cfg.batch_size = result["batch-size"].as<size_t>();
    train_cfg.learning_rate = result["lr"].as<double>();
    train_cfg.weight_decay = result["weight-decay"].as<double>();
    train_cfg.optimizer = result["optimizer"].as<std::string>();
    train_cfg.loss = result["loss"].as<std::string>();
    train_cfg.validation_split = result["val-split"].as<float>();

    sgrn::ml::ModelManifest manifest;
    manifest.name = model_name;
    manifest.backend = "torch";
    manifest.spec = std::move(spec);
    manifest.mlp = std::move(mlp_cfg);

    std::filesystem::create_directories(out_dir);
    const std::string weights_path = (std::filesystem::path(out_dir) / (model_name + ".pt")).string();
    const std::string manifest_path = (std::filesystem::path(out_dir) / (model_name + "_manifest.json")).string();

    fmt::print(
        "Starting training (epochs: {}, batch_size: {}, lr: {})...\n", train_cfg.epochs, train_cfg.batch_size, train_cfg.learning_rate);

    sgrn::ml_torch::Trainer trainer(target_device);
    auto train_res = trainer.trainAndSave(model, x_tensor, y_tensor, train_cfg, manifest, weights_path);
    if (train_res.hasError()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Training failed: {}\n", train_res.error());
        return 1;
    }

    // Save manifest
    std::ofstream m_file(manifest_path);
    if (!m_file.is_open()) {
        fmt::print(stderr, fmt::fg(fmt::color::red), "Failed to save manifest file: {}\n", manifest_path);
        return 1;
    }
    m_file << manifest.toJson(true);
    m_file.close();

    const auto& res = train_res.value();
    fmt::print(fmt::fg(fmt::color::green), "Training completed successfully!\n");
    fmt::print("  Epochs trained  : {}\n", res.epochs_completed);
    fmt::print("  Final Train Loss: {:.6f}\n", res.final_train_loss);
    if (!res.val_losses.empty()) {
        fmt::print("  Final Val Loss  : {:.6f}\n", res.final_val_loss);
    }
    fmt::print("  Model Weights   : {}\n", weights_path);
    fmt::print("  Weights SHA256  : {}\n", manifest.weights_sha256);
    fmt::print("  Manifest JSON   : {}\n", manifest_path);

    return 0;
}
