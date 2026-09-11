#pragma once

#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sgrn::gateway::core
{

struct CacheEntry {
    uint64_t version_built_at{0};
    std::shared_ptr<const std::string> json;
};

/**
 * @brief Unified Lazy JSON Caching Engine.
 *
 * Architectural Relationship:
 * ───────────────────────────
 * Replaces the legacy `DbEntry::cached_json` string buffer.
 * `TreeCacheEngine` provides granular caching down to the leaf node level.
 *
 * - HTTP GET Routes: Uses `get()` to fetch the semantic representation of
 *   any path. If it's cached and the atomic version matches, it returns
 *   the shared pointer instantly.
 * - PlcState: `TreeCacheEngine` reads the `std::atomic<uint64_t> version`
 *   on the `PlcNode` to determine cache invalidation lazily, avoiding
 *   costly dirty flags and mutex locks during PLC memory writes.
 */
class TreeCacheEngine {
public:
    static TreeCacheEngine& instance() {
        static TreeCacheEngine inst;
        return inst;
    }

    std::shared_ptr<const std::string> get(const twin::TreePath& t_path, twin::PlcState& t_state);

    void pin(const twin::TreePath& t_path);
    void unpin(const twin::TreePath& t_path);

    void setMaxCacheEntries(size_t t_max);

    void clear();

private:
    TreeCacheEngine() = default;

    void evictIfOverCapacity();

    std::shared_mutex mutex_;
    std::unordered_map<twin::TreePath, CacheEntry, twin::TreePathHash, twin::TreePathEqual> cache_;
    std::unordered_set<twin::TreePath, twin::TreePathHash, twin::TreePathEqual> pinned_;
    size_t max_cache_entries_{256};
};

} // namespace sgrn::gateway::core
