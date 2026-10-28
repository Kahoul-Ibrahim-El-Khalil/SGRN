#include <sgrn/ml/backend.hpp>
#include <mutex>
#include <unordered_map>

namespace sgrn::ml
{

namespace
{
struct Registry {
    std::mutex mtx;
    std::unordered_map<std::string, BackendFactory::Creator> creators;
};

Registry& getRegistry() {
    static Registry reg;
    return reg;
}
} // namespace

void BackendFactory::registerBackend(std::string_view t_name, Creator t_creator) {
    auto& reg = getRegistry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    reg.creators[std::string(t_name)] = std::move(t_creator);
}

Result<std::unique_ptr<IModelBackend>> BackendFactory::createBackend(std::string_view t_name) {
    auto& reg = getRegistry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    auto it = reg.creators.find(std::string(t_name));
    if (it == reg.creators.end()) {
        return Result<std::unique_ptr<IModelBackend>>::Error("Model backend not found: " + std::string(t_name));
    }
    return it->second();
}

bool BackendFactory::hasBackend(std::string_view t_name) {
    auto& reg = getRegistry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    return reg.creators.find(std::string(t_name)) != reg.creators.end();
}

std::vector<std::string> BackendFactory::registeredBackends() {
    auto& reg = getRegistry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    std::vector<std::string> names;
    names.reserve(reg.creators.size());
    for (const auto& [name, _] : reg.creators) {
        names.push_back(name);
    }
    return names;
}

} // namespace sgrn::ml
