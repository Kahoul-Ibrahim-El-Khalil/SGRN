#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <sgrn/utils/time.hpp>

#include <cstring>
#include <open62541/types_generated.h>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::twin
{

namespace
{

std::string relativePathFromStatePath(const std::string& t_state_path, const DbEntry& p_entry) {
    if (p_entry.name.empty())
        return t_state_path;
    const std::string prefix = p_entry.name + ".";
    if (t_state_path.size() > prefix.size() && t_state_path.compare(0, prefix.size(), prefix) == 0)
        return t_state_path.substr(prefix.size());
    return t_state_path;
}

size_t nodeByteSpan(const PlcNode& t_node) {
    if (t_node.children_.empty()) {
        if (t_node.type_ == s7codec::Type::String || t_node.type_ == s7codec::Type::WString || t_node.type_ == s7codec::Type::XString ||
            t_node.type_ == s7codec::Type::XWString) {
            return static_cast<size_t>(t_node.size_);
        }
        if (t_node.count_ > 1)
            return static_cast<size_t>(s7codec::typeSpanBytes(t_node.type_, static_cast<int>(t_node.count_)).value_or(0));
        return static_cast<size_t>(s7codec::primitiveSize(t_node.type_).value_or(0));
    }
    return static_cast<size_t>(t_node.size_);
}

core::LeafFieldMeta makeLeafMeta(const PlcNode& t_node, int t_elem_ua_type_index) {
    core::LeafFieldMeta meta;
    if (!t_node.children_.empty())
        return meta;

    meta.type = t_node.type_;
    meta.field_size = static_cast<uint32_t>(nodeByteSpan(t_node));
    meta.array_length = t_node.count_ > 1 ? t_node.count_ : 0;
    meta.elem_ua_type_index = t_elem_ua_type_index;
    meta.valid = meta.field_size > 0;
    return meta;
}

std::shared_ptr<std::vector<uint8_t>> snapshotNodeBytes(const PlcNode& t_node, const DbEntry& p_entry, PlcState& t_state) {
    const size_t span = nodeByteSpan(t_node);
    if (span == 0 || t_node.offset_ + span > p_entry.size)
        return nullptr;

    auto bytes = std::make_shared<std::vector<uint8_t>>(span);
    const uint8_t* p_src = t_state.arenaData() + p_entry.offset + t_node.offset_;
    std::memcpy(bytes->data(), p_src, span);
    return bytes;
}

int inferUaTypeIndex(const PlcNode& t_node) {
    switch (t_node.type_) {
        case s7codec::Type::Bool:
            return UA_TYPES_BOOLEAN;
        case s7codec::Type::Byte:
            return UA_TYPES_BYTE;
        case s7codec::Type::Char:
        case s7codec::Type::SInt:
            return UA_TYPES_SBYTE;
        case s7codec::Type::Word:
        case s7codec::Type::UInt:
            return UA_TYPES_UINT16;
        case s7codec::Type::Int:
            return UA_TYPES_INT16;
        case s7codec::Type::DInt:
            return UA_TYPES_INT32;
        case s7codec::Type::DWord:
        case s7codec::Type::UDInt:
            return UA_TYPES_UINT32;
        case s7codec::Type::LInt:
            return UA_TYPES_INT64;
        case s7codec::Type::LWord:
        case s7codec::Type::ULInt:
            return UA_TYPES_UINT64;
        case s7codec::Type::Real:
            return UA_TYPES_FLOAT;
        case s7codec::Type::LReal:
        case s7codec::Type::Time:
            return UA_TYPES_DOUBLE;
        case s7codec::Type::TimeOfDay:
            return UA_TYPES_STRING;
        case s7codec::Type::String:
        case s7codec::Type::WString:
        case s7codec::Type::XString:
        case s7codec::Type::XWString:
            return UA_TYPES_STRING;
        default:
            return UA_TYPES_INT32;
    }
}

} // namespace

FieldUpdateNotification makeFieldUpdateNotification(PlcState& t_state, const PlcNode& t_node, const DbEntry& p_entry,
    const std::string& t_state_path, uint64_t t_timestamp, bool t_include_json) {
    FieldUpdateNotification note;
    note.db = t_node.db_number_;
    note.state_path = t_state_path;
    note.path = relativePathFromStatePath(t_state_path, p_entry);
    note.timestamp = t_timestamp;

    const int ua_idx = inferUaTypeIndex(t_node);
    note.typed_leaf.meta = makeLeafMeta(t_node, ua_idx);
    if (note.typed_leaf.meta.valid)
        note.typed_leaf.bytes = snapshotNodeBytes(t_node, p_entry, t_state);

    if (t_include_json) {
        if (t_node.children_.empty())
            note.json_value = t_state.getScalarString(t_state_path);
        else
            note.json_value = t_state.getJsonString(t_state_path);
    }

    return note;
}

std::vector<FieldUpdateNotification> gatherTypedDirtyLeaves(PlcState& t_state, const std::string& t_segment_name, bool t_include_json) {
    std::vector<FieldUpdateNotification> out;
    const PlcNode* p_root = t_state.find(t_segment_name);
    if (!p_root || !p_root->cached_slot_)
        return out;

    const DbEntry* p_entry = p_root->cached_slot_;
    if (!p_entry->is_dirty_.load(std::memory_order_acquire))
        return out;

    const std::string prefix = t_segment_name + ".";
    const uint64_t t_timestamp = sgrn::utils::time::nowMilliseconds();

    std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);
    // HIGH-2: Snapshot the segment's arena slice under the lock so that
    // makeFieldUpdateNotification reads from a stable copy, not from live
    // arena bytes that a concurrent writeDbMemory call can overwrite.
    std::vector<uint8_t> arena_snapshot(p_entry->size);
    std::memcpy(arena_snapshot.data(), t_state.arenaData() + p_entry->offset, p_entry->size);
    lk.unlock();

    for (const auto& [path, t_node] : t_state.nodes()) {
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (!t_node.children_.empty() || !t_node.cached_slot_)
            continue;
        out.push_back(makeFieldUpdateNotification(t_state, t_node, *p_entry, path, t_timestamp, t_include_json));
    }
    return out;
}

} // namespace sgrn::gateway::twin
