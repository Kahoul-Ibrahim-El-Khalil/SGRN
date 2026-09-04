#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sgrn::gateway::twin
{
class DbSnapshot;

class SnapshotRegistry {
public:
    SnapshotRegistry() = default;
    ~SnapshotRegistry() = default;

    SnapshotRegistry(const SnapshotRegistry&) = delete;
    SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;

    /// Ensures slot storage is sized up to at least max_db_number + 1
    void ensureCapacity(size_t t_max_db_number);

    void registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot);
    void unregisterSnapshot(uint16_t t_db_number);
    void patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes);

private:
    // Direct-index, lock-free on patch hot path: dynamically bounded to max DB number + 1,
    // avoiding static 65,536 slot (512 KB) pre-allocation overhead.
    std::mutex resize_mutex_;
    std::unique_ptr<std::atomic<DbSnapshot*>[]> slots_;
    std::atomic<size_t> capacity_{0};
};
} // namespace sgrn::gateway::twin
