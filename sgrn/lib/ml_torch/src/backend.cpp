#include <sgrn/ml_torch/backend.hpp>
#include <filesystem>

namespace sgrn::ml_torch
{

namespace
{
torch::Device selectDefaultDevice() {
#if defined(TORCH_USE_GPU)
    if (torch::cuda::is_available()) {
        return torch::Device(torch::kCUDA);
    }
    return torch::Device(torch::kCPU);
#elif defined(TORCH_USE_CPU)
    return torch::Device(torch::kCPU);
#else
    if (torch::cuda::is_available()) {
        return torch::Device(torch::kCUDA);
    }
    return torch::Device(torch::kCPU);
#endif
}
} // namespace

TorchBackend::TorchBackend()
    : device_(selectDefaultDevice()) {
}

TorchBackend::TorchBackend(torch::Device t_device)
    : device_(t_device) {
}

void TorchBackend::setDevice(torch::Device t_device) {
    device_ = t_device;
    if (mlp_) {
        mlp_->to(device_);
    }
    if (jit_module_) {
        jit_module_->to(device_);
    }
}

void TorchBackend::setMlp(Mlp t_mlp) {
    mlp_ = std::move(t_mlp);
    is_jit_ = false;
    jit_module_.reset();
    if (mlp_) {
        in_features_ = mlp_->inputSize();
        out_features_ = mlp_->outputSize();
        mlp_->to(device_);
        mlp_->eval();
    }
}

Result<void> TorchBackend::load(const std::string& t_weights_path, const sgrn::ml::ModelManifest& t_manifest) {
    in_features_ = t_manifest.spec.inputSize();
    out_features_ = t_manifest.spec.outputSize();
    if (out_features_ == 0 && !t_manifest.mlp.layers.empty()) {
        out_features_ = t_manifest.mlp.layers.back().units;
    }

    const bool file_exists = !t_weights_path.empty() && std::filesystem::exists(t_weights_path);

    // 1. If manifest defines MLP architecture, instantiate Mlp and load weights archive
    if (!t_manifest.mlp.layers.empty()) {
        try {
            mlp_ = Mlp(t_manifest);
            if (file_exists) {
                torch::load(mlp_, t_weights_path);
            }
            mlp_->to(device_);
            mlp_->eval();
            in_features_ = mlp_->inputSize();
            out_features_ = mlp_->outputSize();
            is_jit_ = false;
            jit_module_.reset();
            return {};
        } catch (const std::exception& ex) {
            mlp_ = nullptr;
        }
    }

    // 2. Try loading as an executable TorchScript module
    if (file_exists) {
        try {
            auto module = torch::jit::load(t_weights_path, device_);
            if (module.find_method("forward")) {
                jit_module_ = std::make_unique<torch::jit::script::Module>(std::move(module));
                jit_module_->eval();
                is_jit_ = true;
                mlp_ = nullptr;
                return {};
            }
        } catch (...) {
            // Not TorchScript
        }
    }

    if (!mlp_ && !jit_module_) {
        return Result<void>::Error("Failed to load model weights from: " + t_weights_path);
    }

    return {};
}

Result<std::vector<float>> TorchBackend::predict(std::span<const float> t_features) {
    if (!mlp_ && !jit_module_) {
        return Result<std::vector<float>>::Error("TorchBackend has no loaded model");
    }

    if (in_features_ > 0 && t_features.size() != in_features_) {
        return Result<std::vector<float>>::Error(fmt::format("Input feature size {} != expected {}", t_features.size(), in_features_));
    }

    try {
        torch::NoGradGuard no_grad;

        auto tensor =
            torch::from_blob(const_cast<float*>(t_features.data()), {1, static_cast<int64_t>(t_features.size())}, torch::kFloat32);

        if (device_.is_cuda()) {
            tensor = tensor.to(device_);
        }

        torch::Tensor out;
        if (is_jit_) {
            std::vector<torch::jit::IValue> inputs;
            inputs.push_back(tensor);
            out = jit_module_->forward(inputs).toTensor();
        } else {
            out = mlp_->forward(tensor);
        }

        out = out.to(torch::kCPU).contiguous();
        const float* data_ptr = out.data_ptr<float>();
        const size_t num_elements = out.numel();

        return std::vector<float>(data_ptr, data_ptr + num_elements);
    } catch (const std::exception& ex) {
        return Result<std::vector<float>>::Error(std::string("TorchBackend predict error: ") + ex.what());
    }
}

void TorchBackend::registerWithFactory() {
    sgrn::ml::BackendFactory::registerBackend("torch", []() { return std::make_unique<TorchBackend>(); });
}

} // namespace sgrn::ml_torch
