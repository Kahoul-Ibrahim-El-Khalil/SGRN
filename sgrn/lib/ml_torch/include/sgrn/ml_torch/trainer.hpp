#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/manifest.hpp>
#include <sgrn/ml_torch/mlp.hpp>
#include <cstddef>
#include <string>
#include <torch/torch.h>
#include <vector>

namespace sgrn::ml_torch
{

struct TrainerConfig {
    size_t epochs = 10;
    size_t batch_size = 32;
    double learning_rate = 1e-3;
    double weight_decay = 1e-4;
    std::string optimizer = "adamw"; // "adamw", "adam", "sgd"
    std::string loss = "mse";        // "mse", "bce", "l1"
    float validation_split = 0.2f;
    bool shuffle = true;
};

struct TrainResult {
    float final_train_loss = 0.0f;
    float final_val_loss = 0.0f;
    std::vector<float> epoch_losses;
    std::vector<float> val_losses;
    size_t epochs_completed = 0;
};

class Trainer {
public:
    explicit Trainer(torch::Device t_device = torch::kCPU);

    Result<TrainResult> train(Mlp& t_model, torch::Tensor t_x, torch::Tensor t_y, const TrainerConfig& t_config);

    Result<void> saveWeights(Mlp& t_model, const std::string& t_weights_path);

    Result<TrainResult> trainAndSave(Mlp& t_model, torch::Tensor t_x, torch::Tensor t_y, const TrainerConfig& t_config,
        sgrn::ml::ModelManifest& t_manifest, const std::string& t_weights_path);

private:
    torch::Device device_{torch::kCPU};
};

} // namespace sgrn::ml_torch
