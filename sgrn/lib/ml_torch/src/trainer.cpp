#include <sgrn/ml_torch/trainer.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/time.hpp>
#include <chrono>

namespace sgrn::ml_torch
{

Trainer::Trainer(torch::Device t_device)
    : device_(t_device) {
}

static torch::Tensor computeLoss(const torch::Tensor& t_pred, const torch::Tensor& t_target, const std::string& t_loss_name) {
    if (t_loss_name == "bce") {
        return torch::binary_cross_entropy_with_logits(t_pred, t_target);
    } else if (t_loss_name == "l1") {
        return torch::l1_loss(t_pred, t_target);
    }
    // Default to MSE
    return torch::mse_loss(t_pred, t_target);
}

Result<TrainResult> Trainer::train(Mlp& t_model, torch::Tensor t_x, torch::Tensor t_y, const TrainerConfig& t_config) {
    if (t_x.size(0) == 0 || t_y.size(0) == 0) {
        return Result<TrainResult>::Error("Cannot train on empty dataset");
    }
    if (t_x.size(0) != t_y.size(0)) {
        return Result<TrainResult>::Error("Input and target sample counts do not match");
    }

    try {
        t_model->to(device_);
        auto x = t_x.to(device_);
        auto y = t_y.to(device_);

        const int64_t n_samples = x.size(0);
        int64_t n_val = 0;
        if (t_config.validation_split > 0.0f && t_config.validation_split < 1.0f) {
            n_val = static_cast<int64_t>(static_cast<float>(n_samples) * t_config.validation_split);
        }
        const int64_t n_train = n_samples - n_val;

        torch::Tensor x_train = x.slice(0, 0, n_train);
        torch::Tensor y_train = y.slice(0, 0, n_train);
        torch::Tensor x_val = n_val > 0 ? x.slice(0, n_train, n_samples) : torch::Tensor();
        torch::Tensor y_val = n_val > 0 ? y.slice(0, n_train, n_samples) : torch::Tensor();

        std::unique_ptr<torch::optim::Optimizer> optimizer;
        if (t_config.optimizer == "sgd") {
            optimizer = std::make_unique<torch::optim::SGD>(
                t_model->parameters(), torch::optim::SGDOptions(t_config.learning_rate).momentum(0.9).weight_decay(t_config.weight_decay));
        } else if (t_config.optimizer == "adam") {
            optimizer = std::make_unique<torch::optim::Adam>(
                t_model->parameters(), torch::optim::AdamOptions(t_config.learning_rate).weight_decay(t_config.weight_decay));
        } else {
            // Default AdamW
            optimizer = std::make_unique<torch::optim::AdamW>(
                t_model->parameters(), torch::optim::AdamWOptions(t_config.learning_rate).weight_decay(t_config.weight_decay));
        }

        TrainResult res;
        res.epoch_losses.reserve(t_config.epochs);
        if (n_val > 0) {
            res.val_losses.reserve(t_config.epochs);
        }

        const int64_t batch_size = std::max<int64_t>(1, static_cast<int64_t>(t_config.batch_size));

        for (size_t epoch = 0; epoch < t_config.epochs; ++epoch) {
            t_model->train();
            double epoch_loss_sum = 0.0;
            int64_t batches = 0;

            torch::Tensor perm = t_config.shuffle ? torch::randperm(n_train, device_) : torch::arange(n_train, device_);

            for (int64_t b = 0; b < n_train; b += batch_size) {
                const int64_t cur_batch = std::min(batch_size, n_train - b);
                const auto indices = perm.slice(0, b, b + cur_batch);
                auto batch_x = x_train.index_select(0, indices);
                auto batch_y = y_train.index_select(0, indices);

                optimizer->zero_grad();
                auto pred = t_model->forward(batch_x);
                auto loss = computeLoss(pred, batch_y, t_config.loss);
                loss.backward();
                optimizer->step();

                epoch_loss_sum += loss.item<double>();
                batches++;
            }

            const float avg_train_loss = batches > 0 ? static_cast<float>(epoch_loss_sum / batches) : 0.0f;
            res.epoch_losses.push_back(avg_train_loss);
            res.final_train_loss = avg_train_loss;

            if (n_val > 0) {
                t_model->eval();
                torch::NoGradGuard no_grad;
                auto val_pred = t_model->forward(x_val);
                auto val_loss = computeLoss(val_pred, y_val, t_config.loss);
                const float v_loss = val_loss.item<float>();
                res.val_losses.push_back(v_loss);
                res.final_val_loss = v_loss;
            }

            res.epochs_completed++;
        }

        return res;
    } catch (const std::exception& ex) {
        return Result<TrainResult>::Error(std::string("Training failed: ") + ex.what());
    }
}

Result<void> Trainer::saveWeights(Mlp& t_model, const std::string& t_weights_path) {
    try {
        t_model->eval();
        torch::save(t_model, t_weights_path);
        return {};
    } catch (const std::exception& ex) {
        return Result<void>::Error(std::string("Failed to save weights: ") + ex.what());
    }
}

Result<TrainResult> Trainer::trainAndSave(Mlp& t_model, torch::Tensor t_x, torch::Tensor t_y, const TrainerConfig& t_config,
    sgrn::ml::ModelManifest& t_manifest, const std::string& t_weights_path) {
    auto train_res = train(t_model, t_x, t_y, t_config);
    if (train_res.hasError()) {
        return train_res;
    }

    auto save_res = saveWeights(t_model, t_weights_path);
    if (save_res.hasError()) {
        return Result<TrainResult>::Error(save_res.error());
    }

    auto hash_res = sgrn::utils::computeSha256File(t_weights_path, sgrn::utils::HashEncoding::hex);
    if (hash_res.hasError()) {
        return Result<TrainResult>::Error("Failed to hash weights file: " + hash_res.error());
    }

    t_manifest.weights_file = t_weights_path;
    t_manifest.weights_sha256 = hash_res.value();
    t_manifest.train.epochs_trained = static_cast<uint32_t>(train_res.value().epochs_completed);
    t_manifest.train.final_loss = train_res.value().final_train_loss;
    t_manifest.train.timestamp_ns = static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds()) * 1'000'000ULL;
    if (!train_res.value().val_losses.empty()) {
        t_manifest.train.metrics["val_loss"] = train_res.value().final_val_loss;
    }

    return train_res;
}

} // namespace sgrn::ml_torch
