#pragma once

#include <sgrn/ml/manifest.hpp>
#include <cstddef>
#include <string>
#include <torch/torch.h>
#include <vector>

namespace sgrn::ml_torch
{

class MlpImpl : public torch::nn::Module {
public:
    MlpImpl(size_t t_in_features, const sgrn::ml::MlpConfig& t_config, size_t t_out_features);
    explicit MlpImpl(const sgrn::ml::ModelManifest& t_manifest);

    torch::Tensor forward(torch::Tensor t_x);

    [[nodiscard]] size_t inputSize() const noexcept {
        return in_features_;
    }
    [[nodiscard]] size_t outputSize() const noexcept {
        return out_features_;
    }

private:
    size_t in_features_ = 0;
    size_t out_features_ = 0;
    torch::nn::Sequential net_{nullptr};
};

TORCH_MODULE(Mlp);

} // namespace sgrn::ml_torch
