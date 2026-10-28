#include <sgrn/ml_torch/mlp.hpp>
#include <algorithm>

namespace sgrn::ml_torch
{

MlpImpl::MlpImpl(const sgrn::ml::ModelManifest& t_manifest)
    : MlpImpl(t_manifest.spec.inputSize(), t_manifest.mlp, t_manifest.spec.outputSize()) {
}

MlpImpl::MlpImpl(size_t t_in_features, const sgrn::ml::MlpConfig& t_config, size_t t_out_features)
    : in_features_(t_in_features)
    , out_features_(t_out_features) {
    net_ = torch::nn::Sequential();

    size_t curr_in = in_features_;

    for (size_t i = 0; i < t_config.layers.size(); ++i) {
        const auto& layer = t_config.layers[i];
        net_->push_back(torch::nn::Linear(curr_in, layer.units));

        if (t_config.use_batch_norm) {
            net_->push_back(torch::nn::BatchNorm1d(layer.units));
        }

        std::string act = layer.activation;
        std::transform(act.begin(), act.end(), act.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (act == "relu") {
            net_->push_back(torch::nn::ReLU());
        } else if (act == "gelu") {
            net_->push_back(torch::nn::GELU());
        } else if (act == "tanh") {
            net_->push_back(torch::nn::Tanh());
        } else if (act == "sigmoid") {
            net_->push_back(torch::nn::Sigmoid());
        }

        if (layer.dropout > 0.0f) {
            net_->push_back(torch::nn::Dropout(layer.dropout));
        }

        curr_in = layer.units;
    }

    if (out_features_ > 0) {
        if (curr_in != out_features_) {
            net_->push_back(torch::nn::Linear(curr_in, out_features_));
        }
    } else {
        out_features_ = curr_in;
    }

    register_module("net", net_);
}

torch::Tensor MlpImpl::forward(torch::Tensor t_x) {
    return net_->forward(t_x);
}

} // namespace sgrn::ml_torch
