// PlcMemory raw-memory access paths (Tier-1 arena + Tier-2 DB-scoped + bit ops).
// Split from PlcMemory.cpp; owns the locking/validation I/O internals.
#include <fmt/core.h>
#include <sgrn/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <asio.hpp>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <shared_mutex>

#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

class PlcState;
struct DbMemorySpan;

// ─────────────────────────────────────────────────────────────────────────────
// Raw memory access
// ─────────────────────────────────────────────────────────────────────────────

void PlcMemory::bumpFieldVersions(
    uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* t_old_base, const uint8_t* t_new_base) {

    p_plc_state_->forEachIntersectingLeaf(t_db_number, t_offset, t_size, [&](PlcNode& node) {
        size_t node_span = node.size_;
        if (node.type_ != s7codec::Type::Struct && node.type_ != s7codec::Type::String && node.type_ != s7codec::Type::WString) {
            node_span = static_cast<size_t>(s7codec::primitiveSize(node.type_).value_or(0)) * std::max(1u, node.count_);
        }

        const size_t intersect_start = std::max(static_cast<size_t>(node.offset_), t_offset);
        const size_t intersect_end = std::min(static_cast<size_t>(node.offset_ + node_span), t_offset + t_size);
        if (intersect_end <= intersect_start)
            return; // forEachIntersectingLeaf's bound is conservative; stay defensive

        const size_t rel = intersect_start - t_offset;
        const size_t len = intersect_end - intersect_start;

        if (std::memcmp(t_old_base + rel, t_new_base + rel, len) != 0) {
            node.bumpVersionChain();
            if (node.state_)
                node.state_->field_dirty_.store(true, std::memory_order_release);
        }
    });
}

PlcMemory::SegmentLookup PlcMemory::findContainingSegment(size_t t_abs_offset, size_t t_size) const {

    auto lookup = p_plc_state_->findSegmentByAbsOffset(t_abs_offset, t_size);

    if (!lookup.p_entry)
        return {nullptr, 0, PlcMemoryError::UNMAPPED_ARENA_REGION};

    if (!lookup.fits)
        return {nullptr, 0, PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY};

    return {lookup.p_entry, t_abs_offset - lookup.p_entry->offset, PlcMemoryError::UNMAPPED_ARENA_REGION};
}

// ── Tier 1: whole arena ──────────────────────────────────────────────────────

Result<void, PlcMemoryError> PlcMemory::read(size_t t_offset, size_t t_size, uint8_t* tp_buffer) const {

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    if (t_size == 0)
        return {};

    if (!tp_buffer)
        return PlcMemoryError::NULL_BUFFER;

    auto lookup = findContainingSegment(t_offset, t_size);

    if (!lookup.p_entry)
        return lookup.error;

    std::shared_lock<std::shared_mutex> lock(lookup.p_entry->mutex_);

    std::memcpy(tp_buffer, p_plc_state_->getArenaTree().data() + lookup.p_entry->offset + lookup.rel_offset, t_size);

    return {};
}

Result<void, PlcMemoryError> PlcMemory::write(size_t t_offset, size_t t_size, const uint8_t* tp_buffer) {

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    if (t_size == 0)
        return {};

    if (!tp_buffer)
        return PlcMemoryError::NULL_BUFFER;

    auto lookup = findContainingSegment(t_offset, t_size);

    if (!lookup.p_entry)
        return lookup.error;

    DbEntry* p_entry = lookup.p_entry;

    uint8_t* target = p_plc_state_->getArenaTree().data() + p_entry->offset + lookup.rel_offset;

    bool changed = false;

    {
        std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);

        if (std::memcmp(target, tp_buffer, t_size) != 0) {

            bumpFieldVersions(static_cast<uint16_t>(p_entry->id), lookup.rel_offset, t_size, target, tp_buffer);

            std::memcpy(target, tp_buffer, t_size);

            changed = true;
        }
    }

    if (changed) {
        p_entry->markDirty();

        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(p_entry->name));

        p_entry->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);

        signalDirty();

        snapshot_registry_->patchSnapshotRegion(static_cast<uint16_t>(p_entry->id), lookup.rel_offset, tp_buffer, t_size);
    }

    return {};
}

namespace
{

struct ResolvedSpan {
    DbEntry* p_entry;
    size_t rel_offset;
    const MemorySpan* p_span;
};

} // namespace

Result<void, PlcMemoryError> PlcMemory::read(std::span<const MemorySpan> t_spans) const {

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    if (t_spans.empty())
        return {};

    std::vector<ResolvedSpan> resolved;
    resolved.reserve(t_spans.size());

    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;

        if (!s.p_buffer)
            return PlcMemoryError::NULL_BUFFER;

        auto lookup = findContainingSegment(s.offset, s.size);

        if (!lookup.p_entry)
            return lookup.error;

        resolved.push_back({lookup.p_entry, lookup.rel_offset, &s});
    }

    std::vector<DbEntry*> touched;
    touched.reserve(resolved.size());

    for (auto& r : resolved)
        touched.push_back(r.p_entry);

    std::sort(touched.begin(), touched.end(), [](DbEntry* tp_a, DbEntry* tp_b) { return tp_a->offset < tp_b->offset; });

    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    // Locked in physical-offset order; std::vector destroys elements in
    // reverse construction order, so these unlock in reverse automatically.
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(touched.size());

    for (auto* e : touched)
        locks.emplace_back(e->mutex_);

    const uint8_t* arena = p_plc_state_->getArenaTree().data();

    for (auto& r : resolved) {
        std::memcpy(r.p_span->p_buffer, arena + r.p_entry->offset + r.rel_offset, r.p_span->size);
    }

    return {};
}

Result<void, PlcMemoryError> PlcMemory::write(std::span<const MemorySpan> t_spans) {

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    if (t_spans.empty())
        return {};

    std::vector<ResolvedSpan> resolved;
    resolved.reserve(t_spans.size());

    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;

        if (!s.p_buffer)
            return PlcMemoryError::NULL_BUFFER;

        auto lookup = findContainingSegment(s.offset, s.size);

        if (!lookup.p_entry)
            return lookup.error;

        resolved.push_back({lookup.p_entry, lookup.rel_offset, &s});
    }

    std::vector<DbEntry*> touched;
    touched.reserve(resolved.size());

    for (auto& r : resolved)
        touched.push_back(r.p_entry);

    std::sort(touched.begin(), touched.end(), [](DbEntry* tp_a, DbEntry* tp_b) { return tp_a->offset < tp_b->offset; });

    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    uint8_t* p_arena = p_plc_state_->getArenaTree().data();

    std::vector<uint8_t> segment_changed(touched.size(), 0);

    {
        std::vector<std::unique_lock<std::shared_mutex>> locks;
        locks.reserve(touched.size());

        for (auto* e : touched)
            locks.emplace_back(e->mutex_);

        // Allocation-free index: touched is 1-3 entries typical, linear scan
        // is faster than hash map and avoids per-batch alloc.
        auto findTouchedIdx = [&](DbEntry* e) -> size_t {
            for (size_t i = 0; i < touched.size(); ++i)
                if (touched[i] == e)
                    return i;
            return 0; // never reached - e is always in touched
        };

        for (auto& r : resolved) {
            uint8_t* target = p_arena + r.p_entry->offset + r.rel_offset;

            if (std::memcmp(target, r.p_span->p_buffer, r.p_span->size) == 0) {
                continue;
            }

            bumpFieldVersions(static_cast<uint16_t>(r.p_entry->id), r.rel_offset, r.p_span->size, target, r.p_span->p_buffer);

            std::memcpy(target, r.p_span->p_buffer, r.p_span->size);

            segment_changed[findTouchedIdx(r.p_entry)] = 1;
        }
    }

    bool any_changed = false;

    for (size_t i = 0; i < touched.size(); ++i) {
        if (!segment_changed[i])
            continue;

        any_changed = true;

        touched[i]->markDirty();

        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(touched[i]->name));

        touched[i]->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);
    }

    if (any_changed) {
        signalDirty();

        for (auto& r : resolved) {
            snapshot_registry_->patchSnapshotRegion(static_cast<uint16_t>(r.p_entry->id), r.rel_offset, r.p_span->p_buffer, r.p_span->size);
        }
    }

    return {};
}

// ── Tier 2: DB-scoped fast path ──────────────────────────────────────────────

Result<void, PlcMemoryError> PlcMemory::readDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) const {

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    if (t_size == 0)
        return {};

    if (!tp_buffer)
        return PlcMemoryError::NULL_BUFFER;

    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    if (!p_entry)
        return PlcMemoryError::DB_SEGMENT_NOT_FOUND;

    if (t_offset > p_entry->size || t_size > p_entry->size - t_offset) {

        return PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE;
    }

    std::shared_lock<std::shared_mutex> lock(p_entry->mutex_);

    std::memcpy(tp_buffer, p_plc_state_->getArenaTree().data() + p_entry->offset + t_offset, t_size);

    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer) {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    SGRN_RETURN_IF(t_size == 0, {});

    SGRN_RETURN_IF_NULL(tp_buffer, PlcMemoryError::NULL_BUFFER);

    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF_NULL(p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    SGRN_RETURN_IF(t_offset > p_entry->size || t_size > p_entry->size - t_offset, PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE);

    uint8_t* target = p_plc_state_->getArenaTree().data() + p_entry->offset + t_offset;

    bool changed = false;

    {
        std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);

        if (std::memcmp(target, tp_buffer, t_size) != 0) {

            bumpFieldVersions(t_db_number, t_offset, t_size, target, tp_buffer);

            std::memcpy(target, tp_buffer, t_size);

            changed = true;
        }
    }

    if (changed) {
        p_entry->markDirty();

        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(p_entry->name));

        p_entry->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);

        signalDirty();

        snapshot_registry_->patchSnapshotRegion(t_db_number, t_offset, tp_buffer, t_size);
    }

    return {};
}

namespace
{

struct ResolvedDbSpan {
    DbEntry* p_entry;
    const DbMemorySpan* span;
};

} // namespace

Result<void, PlcMemoryError> PlcMemory::readDbMemory(std::span<const DbMemorySpan> t_spans) const {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    SGRN_RETURN_IF(t_spans.empty(), {});

    std::vector<ResolvedDbSpan> resolved;
    resolved.reserve(t_spans.size());

    for (const auto& s : t_spans) {
        if (s.size == 0) {
            continue;
        }

        SGRN_RETURN_IF_NULL(s.p_buffer, PlcMemoryError::NULL_BUFFER);

        DbEntry* p_entry = p_plc_state_->findSegmentById(s.db);

        SGRN_RETURN_IF_NULL(p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

        if (s.offset > p_entry->size || s.size > p_entry->size - s.offset) {
            return PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE;
        }

        resolved.push_back({p_entry, &s});
    }

    std::vector<DbEntry*> touched;
    touched.reserve(resolved.size());

    for (auto& r : resolved)
        touched.push_back(r.p_entry);

    std::sort(touched.begin(), touched.end(), [](DbEntry* tp_a, DbEntry* tp_b) { return tp_a->offset < tp_b->offset; });

    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(touched.size());

    for (auto* e : touched)
        locks.emplace_back(e->mutex_);

    const uint8_t* p_arena = p_plc_state_->getArenaTree().data();

    for (auto& r : resolved) {
        std::memcpy(r.span->p_buffer, p_arena + r.p_entry->offset + r.span->offset, r.span->size);
    }

    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeDbMemory(std::span<const DbMemorySpan> t_spans) {

    SGRN_RETURN_IF(!p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);
    SGRN_RETURN_IF(t_spans.empty(), {});

    struct ResolvedDbSpan {
        DbEntry* p_entry;
        const DbMemorySpan* span;
    };

    std::vector<ResolvedDbSpan> resolved;
    resolved.reserve(t_spans.size());

    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;

        SGRN_RETURN_IF_NULL(s.p_buffer, PlcMemoryError::NULL_BUFFER);

        DbEntry* p_entry = p_plc_state_->findSegmentById(s.db);
        SGRN_RETURN_IF_NULL(p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

        SGRN_RETURN_IF(s.offset > p_entry->size || s.size > p_entry->size - s.offset, PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE);

        resolved.push_back({p_entry, &s});
    }

    std::vector<DbEntry*> touched;
    touched.reserve(resolved.size());
    for (auto& r : resolved)
        touched.push_back(r.p_entry);

    std::sort(touched.begin(), touched.end(), [](DbEntry* a, DbEntry* b) { return a->offset < b->offset; });
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    uint8_t* p_arena = p_plc_state_->getArenaTree().data();
    std::vector<uint8_t> segment_changed(touched.size(), 0);

    {
        std::vector<std::unique_lock<std::shared_mutex>> locks;
        locks.reserve(touched.size());
        for (auto* e : touched)
            locks.emplace_back(e->mutex_);

        auto findTouchedIdx = [&](DbEntry* e) -> size_t {
            for (size_t i = 0; i < touched.size(); ++i)
                if (touched[i] == e)
                    return i;
            return 0;
        };

        for (auto& r : resolved) {
            uint8_t* target = p_arena + r.p_entry->offset + r.span->offset;

            if (std::memcmp(target, r.span->p_buffer, r.span->size) == 0)
                continue;

            bumpFieldVersions(static_cast<uint16_t>(r.p_entry->id), r.span->offset, r.span->size, target, r.span->p_buffer);

            std::memcpy(target, r.span->p_buffer, r.span->size);

            segment_changed[findTouchedIdx(r.p_entry)] = 1;
        }
    }

    bool any_changed = false;
    for (size_t i = 0; i < touched.size(); ++i) {
        if (!segment_changed[i])
            continue;
        any_changed = true;
        touched[i]->markDirty();
        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(touched[i]->name));
        touched[i]->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);
    }

    if (any_changed) {
        signalDirty();

        for (auto& r : resolved) {
            snapshot_registry_->patchSnapshotRegion(static_cast<uint16_t>(r.p_entry->id), r.span->offset, r.span->p_buffer, r.span->size);
        }
    }

    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeBit(uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value) {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    SGRN_RETURN_IF(t_bit_index < 0 || t_bit_index > 7, PlcMemoryError::INVALID_BIT_INDEX);

    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF(!p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    SGRN_RETURN_IF(t_byte_offset >= p_entry->size, PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE);

    uint8_t* target = p_plc_state_->getArenaTree().data() + p_entry->offset + t_byte_offset;

    uint8_t old_val;
    uint8_t new_val;

    {
        std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);

        old_val = *target;

        new_val = t_value ? static_cast<uint8_t>(old_val | (1u << t_bit_index)) : static_cast<uint8_t>(old_val & ~(1u << t_bit_index));

        if (new_val == old_val)
            return {};

        *target = new_val;

        p_plc_state_->forEachIntersectingLeaf(t_db_number, t_byte_offset, 1, [&](PlcNode& node) {
            if (node.type_ == s7codec::Type::Bool) {
                if (node.offset_ == t_byte_offset && node.bit_index_ == t_bit_index) {
                    node.bumpVersionChain();
                }
            } else {
                node.bumpVersionChain();
            }
        });

        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(p_entry->name));
    }

    p_entry->markDirty();

    p_entry->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);

    signalDirty();

    return {};
}

} // namespace sgrn::gateway::twin
