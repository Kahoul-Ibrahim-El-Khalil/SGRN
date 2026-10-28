#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/manifest.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::ml
{

class IModelBackend {
public:
    virtual ~IModelBackend() = default;

    [[nodiscard]] virtual Result<void> load(const std::string& t_weights_path, const ModelManifest& t_manifest) = 0;
    [[nodiscard]] virtual Result<std::vector<float>> predict(std::span<const float> t_features) = 0;
    [[nodiscard]] virtual size_t input_size() const = 0;
    [[nodiscard]] virtual size_t output_size() const = 0;
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
};

class BackendFactory {
public:
    using Creator = std::function<std::unique_ptr<IModelBackend>()>;

    static void registerBackend(std::string_view t_name, Creator t_creator);
    static Result<std::unique_ptr<IModelBackend>> createBackend(std::string_view t_name);
    static bool hasBackend(std::string_view t_name);
    static std::vector<std::string> registeredBackends();
};

} // namespace sgrn::ml
