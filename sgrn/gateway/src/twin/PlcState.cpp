#include <sgrn/gateway/twin/PlcState.hpp>
// REVAMP-1: PlcState.cpp — Implementation for PlcState, PlcPoint, and PlcNode.

#include <fmt/format.h>
#include <algorithm>
#include <cstring>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::gateway::twin
{

// PlcPoint implementation removed

// ── PlcState (SECTION 2.3) ────────────────────────────────────────────────

PlcState::PlcState() {
    nodes_.reserve(4096); // Pre-allocate map capacity
}

const PlcNode* PlcState::find(const std::string& t_path) const {
    // Case-insensitive lookup handled by CaseInsensitiveHash/CaseInsensitiveEqual
    auto it = nodes_.find(t_path);
    if (it == nodes_.end())
        return nullptr;
    return &it->second;
}

void PlcState::add(PlcNode t_node, const std::string& t_parent_path, std::optional<size_t> /*fixed_offset*/) {
    std::string full_path = t_parent_path.empty() ? t_node.name_ : t_parent_path + "." + t_node.name_;

    DbEntry* p_entry = nullptr;
    if (t_parent_path.empty()) {
        top_level_names_.push_back(t_node.name_);
        auto it = tree().segments().find(t_node.name_);
        p_entry = it != tree().segments().end() ? it->second.get() : nullptr;
    } else {
        std::string db_name = full_path.substr(0, full_path.find('.'));
        auto it = tree().segments().find(db_name);
        p_entry = it != tree().segments().end() ? it->second.get() : nullptr;
    }

    // recursive_add stores every node in the flat nodes_ map.
    // The second parameter (t_parent_abs_offset) is the DB-absolute byte offset
    // of the parent, so that child nodes stored in the map carry an absolute
    // offset_ rather than a struct-relative one.  The children_ vectors inside
    // each node keep their intra-struct relative offsets because serialization
    // accumulates the offset via the t_extra_offset argument.
    auto recursive_add = [&](auto& t_self, PlcNode& t_n, const std::string& t_path, uint32_t t_parent_abs_offset) -> void {
        std::string f_path = t_path.empty() ? t_n.name_ : t_path + "." + t_n.name_;
        t_n.cached_slot_ = p_entry;
        t_n.full_path_ = f_path; // store for collision verification in find()
        const uint32_t my_abs_offset = t_parent_abs_offset + t_n.offset_;
        for (auto& child : t_n.children_) {
            t_self(t_self, child, f_path, my_abs_offset);
        }
        // Store with absolute offset so processCommands can use it directly.
        PlcNode to_store = t_n;
        to_store.offset_ = my_abs_offset;
        nodes_[f_path] = std::move(to_store);
    };

    recursive_add(recursive_add, t_node, t_parent_path, 0);
}

// getPoint removed

std::string PlcState::getJsonString(const std::string& t_path) const {
    const PlcNode* p_node = find(t_path);
    if (!p_node)
        return "null";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    p_node->serialize(writer, tree());
    return buffer.GetString();
}

std::string PlcState::getScalarString(const std::string& t_path) const {
    // Peel off a trailing "[idx]" if present — nodes are stored without it.
    std::string base_path = t_path;
    int array_index = -1;
    if (!t_path.empty() && t_path.back() == ']') {
        size_t open = t_path.find_last_of('[');
        if (open != std::string::npos) {
            bool ok = true;
            int idx = 0;
            for (size_t i = open + 1; i + 1 < t_path.size(); ++i) {
                char c = t_path[i];
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                idx = idx * 10 + (c - '0');
            }
            if (ok) {
                base_path = t_path.substr(0, open);
                array_index = idx;
            }
        }
    }

    const PlcNode* p_node = find(base_path);
    if (!p_node)
        return "null";
    if (array_index < 0 && (!p_node->children_.empty() || !p_node->cached_slot_))
        return getJsonString(base_path);
    if (!p_node->cached_slot_)
        return "null";

    size_t byte_offset = p_node->offset_;
    int bit_index_ = p_node->bit_index_;
    uint32_t count_ = p_node->count_;

    if (array_index >= 0) {
        if (p_node->type_ == s7codec::Type::Bool && p_node->count_ > 1) {
            byte_offset += static_cast<size_t>(array_index / 8);
            bit_index_ = array_index % 8;
        } else {
            auto elem_size = s7codec::primitiveSize(p_node->type_);
            if (elem_size) {
                byte_offset += static_cast<size_t>(array_index) * static_cast<size_t>(*elem_size);
            }
        }
        count_ = 0; // single element, not the whole array
    }

    const uint8_t* p_ptr = tree().data() + p_node->cached_slot_->offset + byte_offset;
    size_t remaining = p_node->cached_slot_->size - byte_offset;
    auto dv = s7codec::decodeScalar(p_node->type_, p_ptr, remaining, bit_index_, count_, p_node->endian_);
    if (!dv.valid())
        return "null";

    // Enums round-trip as their symbolic name (scalar reads only).
    if (!p_node->enum_map_.empty()) {
        const int64_t numeric = (dv.kind() == s7codec::ValueKind::UnsignedInt) ? static_cast<int64_t>(dv.u())
                                : (dv.kind() == s7codec::ValueKind::SignedInt) ? dv.i()
                                                                               : INT64_MIN;
        if (numeric != INT64_MIN) {
            auto it = p_node->enum_map_.find(static_cast<int>(numeric));
            if (it != p_node->enum_map_.end())
                return it->second;
        }
    }
    return s7codec::formatDecodedValue(dv, p_node->type_);
}

void PlcState::pushCommand(PlcCommand t_cmd) {
    std::lock_guard<std::mutex> lk(command_mutex_);
    commands_.push_back(std::move(t_cmd));
}

std::vector<PlcCommand> PlcState::drainCommands() {
    std::lock_guard<std::mutex> lk(command_mutex_);
    return std::move(commands_);
}

bool PlcState::hasPendingCommands() const {
    std::lock_guard<std::mutex> lk(command_mutex_);
    return !commands_.empty();
}

std::vector<uint16_t> PlcState::topLevelNumbers() const {
    std::vector<uint16_t> out;
    out.reserve(top_level_names_.size());
    for (const auto& t_n : top_level_names_) {
        const auto* sym = find(t_n);
        if (sym && sym->db_number_ > 0)
            out.push_back(sym->db_number_);
    }
    return out;
}

std::vector<std::string> PlcState::names() const {
    // Note: this is slow because we don't store raw strings as keys anymore.
    // However, top_level_names_ is usually what's needed.
    // If we need all names, we'd need another index or store names in PlcNode.
    std::vector<std::string> r;
    for (const auto& [_, t_node] : nodes_)
        r.push_back(t_node.name_); // This only returns the leaf name, not full path
    return r;
}

std::string PlcState::toJson() const {
    return getFullSnapshot();
}

void PlcState::loadFromJson(const std::string& /*root*/) {
    // Omitted
}

std::string PlcState::getFullSnapshot() const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();
    std::vector<uint32_t> top_nums;
    for (auto t_n : topLevelNumbers())
        top_nums.push_back(t_n);
    auto lock = const_cast<::sgrn::ArenaTree&>(tree()).lockSegments(top_nums, LockMode::Shared);

    for (const auto& name : top_level_names_) {
        if (const auto* t_node = find(name)) {
            writer.Key(name.c_str(), static_cast<rapidjson::SizeType>(name.length()));
            rapidjson::StringBuffer db_buffer;
            rapidjson::Writer<rapidjson::StringBuffer> db_writer(db_buffer);
            t_node->serialize(db_writer, tree());

            std::string db_json = db_buffer.GetString();
            writer.RawValue(db_json.c_str(), db_json.length(), rapidjson::kObjectType);
        }
    }
    writer.EndObject();
    return buffer.GetString();
}

std::string PlcState::getFullSnapshotFlat(
    const std::unordered_map<std::string, uint32_t>& t_path_to_id, const std::vector<bool>& t_allowed_by_id) const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();
    std::vector<uint32_t> top_nums;
    for (auto t_n : topLevelNumbers())
        top_nums.push_back(t_n);
    auto lock = const_cast<::sgrn::ArenaTree&>(tree()).lockSegments(top_nums, LockMode::Shared);

    bool logged_missing = false;

    for (const auto& [path, id] : t_path_to_id) {
        if (!t_allowed_by_id.empty()) {
            if (id >= t_allowed_by_id.size() || !t_allowed_by_id[id])
                continue;
        }
        auto it = nodes_.find(path);
        if (it != nodes_.end()) {
            std::string id_str = std::to_string(id);
            writer.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.length()));
            it->second.serialize(writer, tree(), 0, 0);
        } else {
            if (!logged_missing) {
                fmt::print("[PlcState] Warning: some paths from dictionary are missing in PlcState (e.g. {})\n", path);
                logged_missing = true;
            }
        }
    }
    writer.EndObject();
    return buffer.GetString();
}

std::string PlcState::getDeltaSnapshot(const std::vector<uint16_t>& t_filter) const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();

    // Iterate segments by numeric ID — no string comparison for filtering.
    for (const auto& [db_id, p_entry_raw] : tree().segments_by_id()) {
        if (!t_filter.empty() && std::find(t_filter.begin(), t_filter.end(), static_cast<uint16_t>(db_id)) == t_filter.end())
            continue;

        auto* p_entry = const_cast<DbEntry*>(p_entry_raw);
        if (!p_entry)
            continue;

        // Atomic exchange: if dirty, copy only this segment's data
        std::vector<uint8_t> seg_copy;
        bool dirty = false;
        {
            std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);
            const bool is_dirty = p_entry->is_dirty_.load(std::memory_order_acquire);
            if (is_dirty) {
                seg_copy.resize(p_entry->offset + p_entry->size, 0);
                std::memcpy(seg_copy.data() + p_entry->offset, tree().data() + p_entry->offset, p_entry->size);
                p_entry->getAndClearDirty();
                dirty = true;
            }
        }

        if (!dirty)
            continue;

        // Find the segment name for the JSON key (reverse lookup — small N).
        std::string seg_name;
        for (const auto& [name, seg] : tree().segments()) {
            if (seg.get() == p_entry) {
                seg_name = name;
                break;
            }
        }
        if (seg_name.empty())
            continue;

        const auto* t_node = find(seg_name);
        if (!t_node)
            continue;

        writer.Key(seg_name.c_str(), static_cast<rapidjson::SizeType>(seg_name.length()));
        rapidjson::StringBuffer db_buffer;
        rapidjson::Writer<rapidjson::StringBuffer> db_writer(db_buffer);

        struct SegmentArenaView : public ::sgrn::ArenaTree {
            SegmentArenaView(std::vector<uint8_t> data) {
                arena_ = std::move(data);
            }
        };
        SegmentArenaView temp_tree(std::move(seg_copy));

        t_node->serialize(db_writer, temp_tree);

        std::string db_json = db_buffer.GetString();
        writer.RawValue(db_json.c_str(), db_json.length(), rapidjson::kObjectType);
    }

    writer.EndObject();
    return buffer.GetString();
}

void PlcState::clear() {
    nodes_.clear();
    top_level_names_.clear();
    tree().clear();
}

void PlcState::incrementNodeVersion(const TreePath& t_path) {
    std::optional<TreePath> current = t_path;
    while (current) {
        auto it = nodes_.find(current->toDotted());
        if (it != nodes_.end()) {
            it->second.version_.fetch_add(1, std::memory_order_release);
        }
        current = current->parent();
    }
}

std::vector<TreePath> PlcState::dirtyPathsSince(const TreePath& t_scope, uint64_t t_last_seen_version) const {
    std::vector<TreePath> result;

    auto scope_str = t_scope.toDotted();
    auto it = nodes_.find(scope_str);
    if (it == nodes_.end())
        return result;

    if (it->second.version_.load(std::memory_order_acquire) <= t_last_seen_version) {
        return result; // whole subtree is clean
    }

    auto recurse = [&](auto& t_self, const PlcNode& t_current_node, const TreePath& t_current_path) -> void {
        if (t_current_node.version_.load(std::memory_order_acquire) <= t_last_seen_version) {
            return;
        }

        if (t_current_node.children_.empty()) {
            result.push_back(t_current_path);
            return;
        }

        for (const auto& child : t_current_node.children_) {
            std::string child_dotted = t_current_path.empty() ? child.name_ : t_current_path.toDotted() + "." + child.name_;
            auto child_it = nodes_.find(child_dotted);
            if (child_it != nodes_.end()) {
                t_self(t_self, child_it->second, TreePath::fromDotted(child_dotted));
            }
        }
    };

    recurse(recurse, it->second, t_scope);
    return result;
}

} // namespace sgrn::gateway::twin
