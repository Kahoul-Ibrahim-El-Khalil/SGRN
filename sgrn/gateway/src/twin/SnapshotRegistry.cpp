#include <fmt/core.h>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>

namespace sgrn::gateway::twin
{

void SnapshotRegistry::registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot) {
    if (!tp_snapshot)
        return;
    slots_[t_db_number].store(tp_snapshot, std::memory_order_release);
}

void SnapshotRegistry::unregisterSnapshot(uint16_t t_db_number) {
    slots_[t_db_number].store(nullptr, std::memory_order_release);
}

void SnapshotRegistry::patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes) {
    DbSnapshot* p_snap = slots_[t_db_number].load(std::memory_order_acquire);
    if (!p_snap)
        return;
    p_snap->updateRawRegion(t_offset, tp_src, t_bytes);
}

} // namespace sgrn::gateway::twin
