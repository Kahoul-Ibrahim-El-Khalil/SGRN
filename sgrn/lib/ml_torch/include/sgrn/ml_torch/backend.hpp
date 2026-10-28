#pragma once

#include <sgrn/ml/backend.hpp>
#include <sgrn/ml/manifest.hpp>
#include <sgrn/ml_torch/mlp.hpp>
#include <memory>
#include <span>
#include <string>
#include <torch/script.h>
#include <torch/torch.h>
#include <vector>

namespace sgrn::ml_torch
{

class TorchBackend : public sgrn::ml::IModelBackend {
public:
    TorchBackend();
    explicit TorchBackend(torch::Device t_device);

    Result<void> load(const std::string& t_weights_path, const sgrn::ml::ModelManifest& t_manifest) override;
    Result<std::vector<float>> predict(std::span<const float> t_features) override;

    [[nodiscard]] size_t input_size() const override {
        return in_features_;
    }
    [[nodiscard]] size_t output_size() const override {
        return out_features_;
    }
    [[nodiscard]] const char* backend_name() const noexcept override {
        return "torch";
    }

    [[nodiscard]] torch::Device device() const noexcept {
        return device_;
    }
    void setDevice(torch::Device t_device);

    void setMlp(Mlp t_mlp);
    [[nodiscard]] Mlp getMlp() const noexcept {
        return mlp_;
    }

    static void registerWithFactory();

private:
    torch::Device device_{torch::kCPU};
    size_t in_features_ = 0;
    size_t out_features_ = 0;
    Mlp mlp_{nullptr};
    std::unique_ptr<torch::jit::script::Module> jit_module_;
    bool is_jit_ = false;
};

} // namespace sgrn::ml_torch
