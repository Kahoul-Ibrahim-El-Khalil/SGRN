#include <sgrn/gateway/core/TreeCacheEngine.hpp>

namespace sgrn::gateway::core
{

std::shared_ptr<const std::string> TreeCacheEngine::get(const twin::TreePath& t_path, twin::PlcState& t_state) {
    auto path_str = t_path.toDotted();
    auto it = t_state.nodes().find(path_str);
    if (it == t_state.nodes().end() || !it->second) {
        return std::make_shared<const std::string>("null");
    }

    uint64_t current_version = it->second->state_ ? it->second->state_->version_.load(std::memory_order_acquire) : 0;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto cache_it = cache_.find(t_path);
        if (cache_it != cache_.end()) {
            if (cache_it->second.version_built_at == current_version) {
                return cache_it->second.json;
            }
        }
    }

    // Cache miss or stale
    std::string new_json;
    if (it->second->children_.empty()) {
        new_json = t_state.getScalarString(path_str);
    } else {
        new_json = t_state.getJsonString(path_str);
    }

    auto new_shared_json = std::make_shared<const std::string>(std::move(new_json));

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_[t_path] = CacheEntry{current_version, new_shared_json};
        evictIfOverCapacity();
    }

    return new_shared_json;
}

void TreeCacheEngine::evictIfOverCapacity() {
    while (cache_.size() > max_cache_entries_) {
        auto it = cache_.begin();
        while (it != cache_.end() && pinned_.count(it->first) > 0) {
            ++it;
        }
        if (it != cache_.end()) {
            cache_.erase(it);
        } else {
            break;
        }
    }
}

void TreeCacheEngine::setMaxCacheEntries(size_t t_max) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    max_cache_entries_ = t_max;
    evictIfOverCapacity();
}

void TreeCacheEngine::pin(const twin::TreePath& t_path) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pinned_.insert(t_path);
}

void TreeCacheEngine::unpin(const twin::TreePath& t_path) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pinned_.erase(t_path);
}

void TreeCacheEngine::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    cache_.clear();
}

} // namespace sgrn::gateway::core
