#include <iostream>
#include <string>
#include <torch/torch.h>

enum class TargetDevice { Auto, Cpu, Gpu };

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  --cpu               Force CPU execution\n"
              << "  --gpu               Force GPU (CUDA/ROCm) execution\n"
              << "  --device <cpu|cuda> Specify device explicitly\n"
              << "  --help              Show this help message\n";
}

int main(int argc, char** argv) {
    std::cout << "[torch_smoke_test] Initializing LibTorch..." << std::endl;

    TargetDevice target = TargetDevice::Auto;

#if defined(TORCH_USE_GPU)
    target = TargetDevice::Gpu;
    std::cout << "[torch_smoke_test] Compile-time flag: TORCH_USE_GPU enabled." << std::endl;
#elif defined(TORCH_USE_CPU)
    target = TargetDevice::Cpu;
    std::cout << "[torch_smoke_test] Compile-time flag: TORCH_USE_CPU enabled." << std::endl;
#endif

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--cpu") {
            target = TargetDevice::Cpu;
        } else if (arg == "--gpu" || arg == "--cuda" || arg == "--rocm") {
            target = TargetDevice::Gpu;
        } else if (arg == "--device" && i + 1 < argc) {
            std::string dev = argv[++i];
            if (dev == "cpu") {
                target = TargetDevice::Cpu;
            } else if (dev == "cuda" || dev == "gpu" || dev == "rocm") {
                target = TargetDevice::Gpu;
            } else {
                std::cerr << "[torch_smoke_test] Unknown device: " << dev << std::endl;
                return 1;
            }
        } else {
            std::cerr << "[torch_smoke_test] Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    const bool cuda_available = torch::cuda::is_available();
    torch::DeviceType device_type = torch::kCPU;

    if (target == TargetDevice::Gpu) {
        if (!cuda_available) {
            std::cerr << "[torch_smoke_test] ERROR: GPU requested (--gpu / -DTORCH_USE_GPU), "
                      << "but torch::cuda::is_available() is false.\n"
                      << "Ensure CUDA or ROCm driver and compatible LibTorch are installed." << std::endl;
            return 1;
        }
        device_type = torch::kCUDA;
    } else if (target == TargetDevice::Cpu) {
        device_type = torch::kCPU;
    } else {
        // Auto
        if (cuda_available) {
            device_type = torch::kCUDA;
            std::cout << "[torch_smoke_test] Auto-detected GPU (CUDA/ROCm) acceleration." << std::endl;
        } else {
            device_type = torch::kCPU;
            std::cout << "[torch_smoke_test] Defaulting to CPU device." << std::endl;
        }
    }

    torch::Device device(device_type);
    std::cout << "[torch_smoke_test] Selected device: " << (device.is_cuda() ? "CUDA/ROCm (GPU)" : "CPU") << std::endl;

    if (device.is_cuda()) {
        std::cout << "[torch_smoke_test] GPU count: " << torch::cuda::device_count() << std::endl;
    }

    try {
        // 1. Tensor creation on device
        auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
        torch::Tensor x = torch::tensor({{1.0f, 2.0f}, {3.0f, 4.0f}}, options);
        std::cout << "[torch_smoke_test] Created input tensor [2, 2] on " << x.device() << ":\n" << x << std::endl;

        // 2. Linear layer on device
        torch::nn::Linear linear(2, 1);
        linear->to(device);

        torch::Tensor y = linear->forward(x);
        std::cout << "[torch_smoke_test] Forward output [2, 1] on " << y.device() << ":\n" << y << std::endl;

        // 3. Loss & backward pass
        torch::Tensor target_tensor = torch::tensor({{1.0f}, {0.0f}}, options);
        torch::Tensor loss = torch::mse_loss(y, target_tensor);
        std::cout << "[torch_smoke_test] MSE loss: " << loss.item<float>() << std::endl;

        loss.backward();
        std::cout << "[torch_smoke_test] Backward pass completed successfully." << std::endl;

        auto grad = linear->weight.grad();
        if (grad.defined() && grad.numel() == 2) {
            std::cout << "[torch_smoke_test] Weight grad on " << grad.device() << ":\n" << grad << std::endl;
            std::cout << "[torch_smoke_test] Smoke test PASSED on " << (device.is_cuda() ? "CUDA/ROCm (GPU)" : "CPU") << "." << std::endl;
            return 0;
        } else {
            std::cerr << "[torch_smoke_test] Smoke test FAILED: undefined gradient." << std::endl;
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[torch_smoke_test] Exception during test: " << ex.what() << std::endl;
        return 1;
    }
}
