#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace sgrn::gateway::twin
{
class DbSnapshot;

class SnapshotRegistry {
public:
    void registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot);
    void unregisterSnapshot(uint16_t t_db_number);
    void patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes);

private:
    mutable std::mutex snapshot_index_mutex_;
    std::unordered_map<uint16_t, DbSnapshot*> snapshot_index_;
};
} // namespace sgrn::gateway::twin
