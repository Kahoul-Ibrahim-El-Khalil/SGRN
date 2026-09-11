#pragma once

#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    /**
     * @brief Cached full-twin JSON (the `/data/` root document).
     *
     * There is no synthetic root node, so validity is derived: every twin
     * write bumps versions up the whole ancestor chain (bumpVersionChain),
     * hence a snapshot of per-DB (name, version) pairs fully determines
     * freshness. A mismatch — or a changed DB set after a schema reload —
     * rebuilds via PlcState::getFullSnapshot(). Lives outside the bounded
     * path cache so the hot root neither evicts field entries nor is evicted
     * by them. Thread-safe; concurrent rebuilds are benign (last wins, and
     * any serialized state is a valid snapshot).
     */
    std::shared_ptr<const std::string> getRoot(twin::PlcState& t_state);

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

    struct RootCache {
        std::vector<std::pair<std::string, uint64_t>> db_versions;
        std::shared_ptr<const std::string> json;
    };
    std::mutex root_mutex_;
    RootCache root_cache_;
};

} // namespace sgrn::gateway::core
