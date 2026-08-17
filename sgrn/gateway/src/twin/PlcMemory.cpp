#include <fmt/core.h>
#include <sgrn/gateway/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <asio.hpp>
#include <cstring>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <shared_mutex>

#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>
#include <sgrn/gateway/twin/field_update.hpp>

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;
using ::sgrn::scl::Error;
using ::sgrn::scl::ErrorCode;
using ::sgrn::scl::SchemaCode;
class PlcState;
struct DbMemorySpan;
PlcMemory::PlcMemory()
    : cmd_processor_(std::make_unique<PlcCommandProcessor>(*this))
    , snapshot_registry_(std::make_unique<SnapshotRegistry>()) {
}

PlcMemory::~PlcMemory() = default;

void PlcMemory::attachState(PlcState& t_state) {
    p_plc_state_ = &t_state;
    if (p_plc_state_)
        p_plc_state_->setCacheEnabled(is_cache_enabled_); // NOTE: check the actual member name — see below
}

PlcState* PlcMemory::state() const {
    return p_plc_state_;
}

const PlcNode* PlcMemory::findSymbol(const std::string& t_path) const {
    return p_plc_state_ ? p_plc_state_->find(t_path) : nullptr;
}
Result<void, ::sgrn::scl::Error> PlcMemory::loadRegistry(const PlcSchemaStore& t_store) {
    std::lock_guard<std::mutex> lock(dirty_cv_mutex_);
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "PlcState not attached"};
    }

    uint16_t next_id = 1000;
    auto convert_to_node = [&](const DbField& t_field, auto& t_self_ref, DbEntry* tentry, s7codec::Endian t_db_endian) -> PlcNode {
        PlcNode n;
        n.name_ = t_field.name;
        n.id_ = next_id++;
        n.cached_slot_ = tentry;
        n.endian_ = t_field.endianness;
        n.is_dynamic_ = t_field.is_dynamic;

        if (t_field.type == DataType::Struct) {
            n.size_ = t_field.struct_size;
        } else if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
                   t_field.type == DataType::XWString) {
            int max_len = (t_field.struct_size > 0 ? t_field.struct_size : t_field.count);
            n.size_ = static_cast<size_t>(s7codec::typeSpanBytes(t_field.type, max_len));
        } else {
            n.size_ = static_cast<size_t>(s7codec::primitiveSize(t_field.type));
        }

        switch (t_field.type) {
            case DataType::Bool:
                n.universal_type_ = sgrn::UniversalType::Bool;
                break;
            case DataType::Real:
            case DataType::LReal:
                n.universal_type_ = sgrn::UniversalType::Float;
                break;
            case DataType::SInt:
            case DataType::Int:
            case DataType::DInt:
            case DataType::LInt:
                n.universal_type_ = sgrn::UniversalType::Int;
                break;
            case DataType::Byte:
            case DataType::USInt:
            case DataType::Word:
            case DataType::UInt:
            case DataType::DWord:
            case DataType::UDInt:
            case DataType::LWord:
            case DataType::ULInt:
                n.universal_type_ = sgrn::UniversalType::UInt;
                break;
            case DataType::String:
            case DataType::WString:
            case DataType::XString:
            case DataType::XWString:
                n.universal_type_ = sgrn::UniversalType::String;
                break;
            case DataType::DateTime:
            case DataType::Date:
            case DataType::TimeOfDay:
            case DataType::Time:
                n.universal_type_ = sgrn::UniversalType::DateTime;
                break;
            default:
                n.universal_type_ = sgrn::UniversalType::Unknown;
                break;
        }

        n.offset_ = static_cast<uint32_t>(t_field.offset);
        n.bit_index_ = static_cast<uint8_t>(t_field.bit_index);
        n.type_ = t_field.type;
        n.count_ = static_cast<uint32_t>(t_field.count);
        for (const auto& child : t_field.children) {
            n.children_.push_back(t_self_ref(child, t_self_ref, tentry, t_db_endian));
        }
        return n;
    };

    for (uint16_t num : t_store.availableDbs()) {
        auto res = t_store.getDb(num);
        if (res.hasError() || !res.value() || res.value()->size_bytes <= 0)
            continue;

        const auto* schema = res.value();
        std::string db_path = schema->db_name.empty() ? fmt::format("DB{}", num) : schema->db_name;

        // NOTE: previously called registerBlock(*core_block) here, inherited
        // from ::sgrn::MemoryProvider. Removed: PlcMemory no longer derives
        // from MemoryProvider (see PlcMemory.hpp). Nothing in gateway/ or
        // s7shell/ ever read from MemoryProvider::registry_ — it was a
        // second, parallel ArenaTree that only ever received metadata
        // (segment/leaf offsets) and never a single real byte, since all
        // actual reads/writes always went through plc_state_'s arena below.
        // If a repo-wide `grep -rn "\.registry()"` turns up a real consumer
        // outside these two files, reinstate a private (non-inherited)
        // Registry member here and feed it the same way.

        p_plc_state_->registerSegment(db_path, num, schema->size_bytes);
        DbEntry* p_entry = p_plc_state_->findSegmentById(num);
        if (!p_entry)
            continue;

        PlcNode n;
        n.name_ = db_path;
        n.size_ = schema->size_bytes;
        n.cached_slot_ = p_entry;
        n.db_number_ = num;
        n.endian_ = schema->endianness;
        for (const auto& t_field : schema->fields) {
            n.children_.push_back(convert_to_node(t_field, convert_to_node, p_entry, schema->endianness));
        }
        p_plc_state_->add(std::move(n), "");
    }

    return {};
}

Result<void, ::sgrn::scl::Error> PlcMemory::registerDb(uint16_t t_db_number, size_t t_size) {
    std::lock_guard<std::mutex> lock(dirty_cv_mutex_);
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "PlcState not attached"};
    }

    std::string path = fmt::format("DB{}", t_db_number);

    p_plc_state_->registerSegment(path, t_db_number, t_size);
    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry) {
        return Error{SchemaCode::Generic, "Failed to allocate DB in PlcArena"};
    }

    // (registerBlock() removed here too — see note in loadRegistry() above.)

    PlcNode node;
    node.name_ = path;
    node.id_ = t_db_number;
    node.size_ = t_size;
    node.cached_slot_ = p_entry;
    node.db_number_ = t_db_number;

    p_plc_state_->add(std::move(node), "");
    return {};
}

Result<void, ::sgrn::scl::Error> PlcMemory::updateField(
    uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json) {
    const uint64_t ts = timestamp_provider_ ? timestamp_provider_() : static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());
    return updateFieldWithTimestamp(t_db_number, t_field_path, t_value_json, ts);
}

Result<void, ::sgrn::scl::Error> PlcMemory::updateFieldWithTimestamp(
    uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json, uint64_t t_timestamp) {
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "PlcState not attached"};
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry) {
        return Error{SchemaCode::Generic, fmt::format("DB{} not registered", t_db_number)};
    }
    std::string path = p_entry->name;
    if (!t_field_path.empty()) {
        path += ".";
        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');
        path += sub;
    }
    PlcCommand cmd;
    cmd.type = PlcCommand::WriteField;
    cmd.path = path;
    cmd.value_json = t_value_json;
    cmd.timestamp = t_timestamp;
    p_plc_state_->pushCommand(std::move(cmd));
    signalDirty();
    return {};
}

Result<std::string, ::sgrn::scl::Error> PlcMemory::getFieldValue(uint16_t t_db_number, const std::string& t_field_path) const {
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "No state"};
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry) {
        return Error{SchemaCode::Generic, fmt::format("DB{} not registered", t_db_number)};
    }
    std::string path = p_entry->name;
    if (!t_field_path.empty()) {
        path += ".";
        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');
        path += sub;
    }
    std::string val = p_plc_state_->getScalarString(path);
    if (val == "null") {
        return Error{SchemaCode::Generic, fmt::format("Field '{}' not found", path)};
    }
    return val;
}

const PlcNode* PlcMemory::findSymbol(uint16_t t_db_number, const std::string& t_field_path) const {
    if (!p_plc_state_) {
        return nullptr;
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry || p_entry->name.empty()) {
        return nullptr;
    }
    std::string path = p_entry->name;
    if (!t_field_path.empty()) {
        path += ".";
        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');
        path += sub;
    }
    return p_plc_state_->find(path);
}

Result<std::string, ::sgrn::scl::Error> PlcMemory::getDbJson(uint16_t t_db_number) const {
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "No state"};
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry || p_entry->name.empty()) {
        return Error{SchemaCode::Generic, fmt::format("DB{} not registered", t_db_number)};
    }
    std::string val = p_plc_state_->getJsonString(p_entry->name);
    if (val == "null") {
        return Error{SchemaCode::Generic, fmt::format("DB{} ({}) not found in PlcState", t_db_number, p_entry->name)};
    }
    return val;
}

Result<std::string, ::sgrn::scl::Error> PlcMemory::getSubtreeJson(uint16_t t_db_number, const std::string& t_field_path) const {
    if (!p_plc_state_) {
        return Error{SchemaCode::Generic, "No state"};
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry || p_entry->name.empty()) {
        return Error{SchemaCode::Generic, fmt::format("DB{} not registered", t_db_number)};
    }
    if (t_field_path.empty()) {
        return getDbJsonString(t_db_number);
    }
    const std::string path = p_entry->name + "." + t_field_path;
    const std::string val = p_plc_state_->getJsonString(path);
    if (val == "null") {
        return Error{SchemaCode::Generic, fmt::format("Subtree '{}' not found", path)};
    }
    return val;
}

std::string PlcMemory::getDbJsonString(uint16_t t_db_number) const {
    if (!p_plc_state_) {
        return "{}";
    }
    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry || p_entry->name.empty()) {
        return "{}";
    }
    const PlcNode* node = p_plc_state_->find(p_entry->name);
    if (!node) {
        return "{}";
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    {
        std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);
        node->serialize(writer, p_plc_state_->getArenaTree());
        return sb.GetString();
    }
}

std::string PlcMemory::getMemoryLayoutAsJson() const {
    return getDigitalTwinJson();
}
std::string PlcMemory::getDigitalTwinJson() const {
    return p_plc_state_ ? p_plc_state_->toJson() : "{}";
}
std::string PlcMemory::getDigitalTwinJsonString() const {
    return p_plc_state_ ? p_plc_state_->getFullSnapshot() : "{}";
}
std::vector<uint8_t> PlcMemory::getFullPlantSnapshot() const {
    if (!p_plc_state_)
        return {};
    auto& arena = p_plc_state_->getArenaTree();
    return std::vector<uint8_t>(arena.data(), arena.data() + arena.size());
}

bool PlcMemory::checkDirty() {
    if (!p_plc_state_)
        return false;
    for (auto& [name_, seg] : p_plc_state_->segments())
        if (seg->is_dirty_.load(std::memory_order_acquire))
            return true;
    return false;
}

std::string PlcMemory::getDeltaSnapshot(const std::vector<std::string>& t_filter) {
    return p_plc_state_ ? p_plc_state_->getDeltaSnapshot(t_filter) : "{}";
}
std::vector<FieldUpdateNotification> PlcMemory::collectTypedDirtyLeaves(const std::string& t_segment_name) {
    return p_plc_state_ ? gatherTypedDirtyLeaves(*p_plc_state_, t_segment_name) : std::vector<FieldUpdateNotification>{};
}
std::vector<std::string> PlcMemory::getDirtyPaths() const {
    std::vector<std::string> out;
    if (!p_plc_state_)
        return out;
    for (auto& [name_, seg] : p_plc_state_->segments())
        if (seg->is_dirty_.load(std::memory_order_acquire))
            out.push_back(seg->name);
    return out;
}
bool PlcMemory::waitForDirty(int t_timeout_ms) {
    std::unique_lock<std::mutex> lock(dirty_cv_mutex_);
    bool triggered = dirty_cv_.wait_for(lock, std::chrono::milliseconds(t_timeout_ms), [this] { return dirty_flag_; });
    dirty_flag_ = false;
    return triggered;
}
void PlcMemory::signalDirty() {
    cmd_processor_->signalDirty();
}

// ─────────────────────────────────────────────────────────────────────────
// Raw memory access
// ─────────────────────────────────────────────────────────────────────────

void PlcMemory::bumpFieldVersions(
    uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* told_base, const uint8_t* tp_new_base) {
    for (const auto& [path, node] : p_plc_state_->nodes()) {
        if (node.db_number_ != t_db_number || !node.children_.empty())
            continue;

        size_t node_span = node.size_;
        if (node.type_ != s7codec::Type::Struct && node.type_ != s7codec::Type::String && node.type_ != s7codec::Type::WString) {
            node_span = s7codec::primitiveSize(node.type_) * std::max(1u, node.count_);
        }

        if (node.offset_ < t_offset + t_size && node.offset_ + node_span > t_offset) {
            size_t intersect_start = std::max(static_cast<size_t>(node.offset_), t_offset);
            size_t intersect_end = std::min(static_cast<size_t>(node.offset_ + node_span), t_offset + t_size);
            size_t rel = intersect_start - t_offset;
            size_t len = intersect_end - intersect_start;
            if (std::memcmp(told_base + rel, tp_new_base + rel, len) != 0)
                p_plc_state_->incrementNodeVersion(TreePath::fromDotted(path));
        }
    }
}

PlcMemory::SegmentLookup PlcMemory::findContainingSegment(size_t t_abs_offset, size_t t_size) const {
    for (const auto& [name_, seg] : p_plc_state_->segments()) {
        DbEntry* p_entry = seg.get();
        if (t_abs_offset >= p_entry->offset && t_abs_offset < p_entry->offset + p_entry->size) {
            if (t_size > p_entry->size - (t_abs_offset - p_entry->offset))
                return {nullptr, 0, PlcMemoryErrorStatus::RANGE_CROSSES_SEGMENT_BOUNDARY};
            return {p_entry, t_abs_offset - p_entry->offset, PlcMemoryErrorStatus::UNMAPPED_ARENA_REGION};
        }
    }
    return {nullptr, 0, PlcMemoryErrorStatus::UNMAPPED_ARENA_REGION};
}

// ── Tier 1: whole arena ──────────────────────────────────────────────────

Result<void, PlcMemoryError> PlcMemory::read(size_t t_offset, size_t t_size, uint8_t* tp_buffer) const {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_size == 0)
        return {};
    if (!tp_buffer)
        return PlcMemoryErrorStatus::NULL_BUFFER;

    auto lookup = findContainingSegment(t_offset, t_size);
    if (!lookup.p_entry)
        return lookup.error;

    std::shared_lock<std::shared_mutex> lock(lookup.p_entry->mutex_);
    std::memcpy(tp_buffer, p_plc_state_->getArenaTree().data() + lookup.p_entry->offset + lookup.rel_offset, t_size);
    return {};
}

Result<void, PlcMemoryError> PlcMemory::write(size_t t_offset, size_t t_size, const uint8_t* tp_buffer) {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_size == 0)
        return {};
    if (!tp_buffer)
        return PlcMemoryErrorStatus::NULL_BUFFER;

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
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_spans.empty())
        return {};

    std::vector<ResolvedSpan> resolved;
    resolved.reserve(t_spans.size());
    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;
        if (!s.p_buffer)
            return PlcMemoryErrorStatus::NULL_BUFFER;
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
    for (auto& r : resolved)
        std::memcpy(r.p_span->p_buffer, arena + r.p_entry->offset + r.rel_offset, r.p_span->size);

    return {};
}

Result<void, PlcMemoryError> PlcMemory::write(std::span<const MemorySpan> t_spans) {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_spans.empty())
        return {};

    std::vector<ResolvedSpan> resolved;
    resolved.reserve(t_spans.size());
    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;
        if (!s.p_buffer)
            return PlcMemoryErrorStatus::NULL_BUFFER;
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

        for (auto& r : resolved) {
            uint8_t* target = p_arena + r.p_entry->offset + r.rel_offset;
            if (std::memcmp(target, r.p_span->p_buffer, r.p_span->size) == 0)
                continue;
            bumpFieldVersions(static_cast<uint16_t>(r.p_entry->id), r.rel_offset, r.p_span->size, target, r.p_span->p_buffer);
            std::memcpy(target, r.p_span->p_buffer, r.p_span->size);
            auto idx = static_cast<size_t>(std::distance(touched.begin(), std::find(touched.begin(), touched.end(), r.p_entry)));
            segment_changed[idx] = 1;
        }
    } // locks released here, in reverse acquisition order

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
        for (auto& r : resolved)
            snapshot_registry_->patchSnapshotRegion(static_cast<uint16_t>(r.p_entry->id), r.rel_offset, r.p_span->p_buffer, r.p_span->size);
    }
    return {};
}

// ── Tier 2: DB-scoped fast path ──────────────────────────────────────────

Result<void, PlcMemoryError> PlcMemory::readDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) const {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_size == 0)
        return {};
    if (!tp_buffer)
        return PlcMemoryErrorStatus::NULL_BUFFER;
    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry)
        return PlcMemoryErrorStatus::DB_SEGMENT_NOT_FOUND;
    if (t_offset > p_entry->size || t_size > p_entry->size - t_offset)
        return PlcMemoryErrorStatus::RANGE_EXCEEDS_ALLOWED_SPACE;

    std::shared_lock<std::shared_mutex> lock(p_entry->mutex_);
    std::memcpy(tp_buffer, p_plc_state_->getArenaTree().data() + p_entry->offset + t_offset, t_size);
    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer) {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_size == 0)
        return {};
    if (!tp_buffer)
        return PlcMemoryErrorStatus::NULL_BUFFER;
    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry)
        return PlcMemoryErrorStatus::DB_SEGMENT_NOT_FOUND;
    if (t_offset > p_entry->size || t_size > p_entry->size - t_offset)
        return PlcMemoryErrorStatus::RANGE_EXCEEDS_ALLOWED_SPACE;

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
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_spans.empty())
        return {};

    std::vector<ResolvedDbSpan> resolved;
    resolved.reserve(t_spans.size());
    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;
        if (!s.p_buffer)
            return PlcMemoryErrorStatus::NULL_BUFFER;
        DbEntry* p_entry = p_plc_state_->findSegmentById(s.db);
        if (!p_entry) {
            return PlcMemoryErrorStatus::DB_SEGMENT_NOT_FOUND;
        }
        if (s.offset > p_entry->size || s.size > p_entry->size - s.offset) {
            return PlcMemoryErrorStatus::RANGE_EXCEEDS_ALLOWED_SPACE;
        }
        resolved.push_back({p_entry, &s});
    }

    std::vector<DbEntry*> touched;
    touched.reserve(resolved.size());
    for (auto& r : resolved) {
        touched.push_back(r.p_entry);
    }
    std::sort(touched.begin(), touched.end(), [](DbEntry* tp_a, DbEntry* tp_b) { return tp_a->offset < tp_b->offset; });
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(touched.size());
    for (auto* e : touched)
        locks.emplace_back(e->mutex_);

    const uint8_t* p_arena = p_plc_state_->getArenaTree().data();
    for (auto& r : resolved)
        std::memcpy(r.span->p_buffer, p_arena + r.p_entry->offset + r.span->offset, r.span->size);

    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeDbMemory(std::span<const DbMemorySpan> t_spans) {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_spans.empty())
        return {};

    std::vector<ResolvedDbSpan> resolved;
    resolved.reserve(t_spans.size());
    for (const auto& s : t_spans) {
        if (s.size == 0)
            continue;
        if (!s.p_buffer)
            return PlcMemoryErrorStatus::NULL_BUFFER;
        DbEntry* p_entry = p_plc_state_->findSegmentById(s.db);
        if (!p_entry)
            return PlcMemoryErrorStatus::DB_SEGMENT_NOT_FOUND;
        if (s.offset > p_entry->size || s.size > p_entry->size - s.offset)
            return PlcMemoryErrorStatus::RANGE_EXCEEDS_ALLOWED_SPACE;
        resolved.push_back({p_entry, &s});
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

        for (auto& r : resolved) {
            uint8_t* target = p_arena + r.p_entry->offset + r.span->offset;
            if (std::memcmp(target, r.span->p_buffer, r.span->size) == 0)
                continue;
            bumpFieldVersions(static_cast<uint16_t>(r.p_entry->id), r.span->offset, r.span->size, target, r.span->p_buffer);
            std::memcpy(target, r.span->p_buffer, r.span->size);
            auto idx = static_cast<size_t>(std::distance(touched.begin(), std::find(touched.begin(), touched.end(), r.p_entry)));
            segment_changed[idx] = 1;
            // MED-2: Patch SnapshotRegistry while the segment lock is still held.
            snapshot_registry_->patchSnapshotRegion(static_cast<uint16_t>(r.p_entry->id), r.span->offset, r.span->p_buffer, r.span->size);
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
    if (any_changed)
        signalDirty();
    return {};
}

Result<void, PlcMemoryError> PlcMemory::writeBit(uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value) {
    if (!p_plc_state_)
        return PlcMemoryErrorStatus::PLC_STATE_NOT_INITIALIZED;
    if (t_bit_index < 0 || t_bit_index > 7)
        return PlcMemoryErrorStatus::INVALID_BIT_INDEX;
    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);
    if (!p_entry)
        return PlcMemoryErrorStatus::DB_SEGMENT_NOT_FOUND;
    if (t_byte_offset >= p_entry->size)
        return PlcMemoryErrorStatus::RANGE_EXCEEDS_ALLOWED_SPACE;

    uint8_t* target = p_plc_state_->getArenaTree().data() + p_entry->offset + t_byte_offset;
    uint8_t old_val, new_val;
    {
        std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);
        old_val = *target;
        new_val = t_value ? static_cast<uint8_t>(old_val | (1u << t_bit_index)) : static_cast<uint8_t>(old_val & ~(1u << t_bit_index));
        if (new_val == old_val)
            return {};
        *target = new_val;

        for (const auto& [path, node] : p_plc_state_->nodes()) {
            if (node.db_number_ != t_db_number || !node.children_.empty())
                continue;
            if (node.offset_ <= t_byte_offset && node.offset_ + node.size_ > t_byte_offset) {
                if (node.type_ == s7codec::Type::Bool) {
                    if (node.offset_ == t_byte_offset && node.bit_index_ == t_bit_index)
                        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(path));
                } else {
                    p_plc_state_->incrementNodeVersion(TreePath::fromDotted(path));
                }
            }
        }
        p_plc_state_->incrementNodeVersion(TreePath::fromDotted(p_entry->name));
    }
    p_entry->markDirty();
    p_entry->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);
    signalDirty();
    return {};
}

} // namespace sgrn::gateway::twin
