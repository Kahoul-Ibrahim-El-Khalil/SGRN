#include <sgrn/crud/FilterHookRegistry.hpp>

namespace sgrn::crud
{

FilterHookRegistry& FilterHookRegistry::instance() {
    static FilterHookRegistry inst;
    return inst;
}

void FilterHookRegistry::registerHook(const std::string& name, FilterHookFunc hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_[name] = std::move(hook);
}

FilterHookFunc FilterHookRegistry::getHook(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hooks_.find(name);
    if (it != hooks_.end()) {
        return it->second;
    }
    return nullptr;
}

bool FilterHookRegistry::hasHook(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_.find(name) != hooks_.end();
}

} // namespace sgrn::crud
