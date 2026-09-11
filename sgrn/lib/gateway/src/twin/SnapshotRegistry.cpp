#include <fmt/core.h>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>

namespace sgrn::gateway::twin
{

void SnapshotRegistry::ensureCapacity(size_t t_max_db_number) {
    std::lock_guard<std::mutex> lock(resize_mutex_);
    const size_t needed = t_max_db_number + 1;
    const size_t current_cap = capacity_.load(std::memory_order_relaxed);
    if (needed <= current_cap)
        return;

    auto new_slots = std::make_unique<std::atomic<DbSnapshot*>[]>(needed);
    for (size_t i = 0; i < needed; ++i) {
        if (i < current_cap && slots_) {
            new_slots[i].store(slots_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            new_slots[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    slots_ = std::move(new_slots);
    capacity_.store(needed, std::memory_order_release);
}

void SnapshotRegistry::registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot) {
    if (!tp_snapshot)
        return;
    if (t_db_number >= capacity_.load(std::memory_order_acquire)) {
        ensureCapacity(t_db_number);
    }
    slots_[t_db_number].store(tp_snapshot, std::memory_order_release);
}

void SnapshotRegistry::unregisterSnapshot(uint16_t t_db_number) {
    const size_t cap = capacity_.load(std::memory_order_acquire);
    if (t_db_number < cap && slots_) {
        slots_[t_db_number].store(nullptr, std::memory_order_release);
    }
}

void SnapshotRegistry::patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes) {
    const size_t cap = capacity_.load(std::memory_order_acquire);
    if (t_db_number >= cap || !slots_)
        return;
    DbSnapshot* p_snap = slots_[t_db_number].load(std::memory_order_acquire);
    if (!p_snap)
        return;
    p_snap->updateRawRegion(t_offset, tp_src, t_bytes);
}

} // namespace sgrn::gateway::twin
