#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sgrn::gateway::twin
{
class DbSnapshot;

class SnapshotRegistry {
public:
    SnapshotRegistry() {
        for (auto& s : slots_)
            s.store(nullptr, std::memory_order_relaxed);
    }
    void registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot);
    void unregisterSnapshot(uint16_t t_db_number);
    void patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes);

private:
    // Direct-index, lock-free: 64k slots, atomic load/store, no mutex,
    // no hash, no alloc on patch (hot path). Register/unregister are
    // rare (polling setup) and do relaxed store; patch does acquire load.
    std::array<std::atomic<DbSnapshot*>, 65536> slots_;
};
} // namespace sgrn::gateway::twin
