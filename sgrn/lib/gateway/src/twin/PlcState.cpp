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
    if (it == nodes_.end() || !it->second)
        return nullptr;
    return it->second.get();
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
        // Stable heap allocation: map rehash moves unique_ptr, not the PlcNode itself.
        auto to_store = std::make_unique<PlcNode>(t_n);
        to_store->offset_ = my_abs_offset;
        // Allocate fresh state for this flat entry (descriptor copy is state-free).
        auto st = std::make_unique<PlcNodeState>();
        to_store->state_ = st.get();
        state_storage_.push_back(std::move(st));
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
        if (t_node)
            r.push_back(t_node->name_); // This only returns the leaf name, not full path
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
        if (it != nodes_.end() && it->second) {
            std::string id_str = std::to_string(id);
            writer.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.length()));
            it->second->serialize(writer, tree(), 0, 0);
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

        // Zero-copy: serialize directly from arena while holding shared lock,
        // no seg_copy allocation/memcpy. Dirty is cleared while still holding
        // the lock so no write is lost between snapshot and clear.
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

        std::string db_json;
        {
            std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);
            if (!p_entry->is_dirty_.load(std::memory_order_acquire))
                continue;
            rapidjson::StringBuffer db_buffer;
            rapidjson::Writer<rapidjson::StringBuffer> db_writer(db_buffer);
            t_node->serialize(db_writer, tree());
            db_json = db_buffer.GetString();
            p_entry->getAndClearDirty();
        }

        writer.Key(seg_name.c_str(), static_cast<rapidjson::SizeType>(seg_name.length()));
        writer.RawValue(db_json.c_str(), db_json.length(), rapidjson::kObjectType);
    }

    writer.EndObject();
    return buffer.GetString();
}

std::string PlcState::getDeltaSnapshotFlat(
    const ankerl::unordered_dense::map<std::string, uint32_t>& t_path_to_id, const std::vector<uint16_t>& t_filter) const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();

    // Mirror the same dirty-segment sweep as getDeltaSnapshot() — zero-copy.
    for (const auto& [db_id, p_entry_raw] : tree().segments_by_id()) {
        if (!t_filter.empty() && std::find(t_filter.begin(), t_filter.end(), static_cast<uint16_t>(db_id)) == t_filter.end())
            continue;

        auto* p_entry = const_cast<DbEntry*>(p_entry_raw);
        if (!p_entry)
            continue;

        std::string seg_name;
        for (const auto& [name, seg] : tree().segments()) {
            if (seg.get() == p_entry) {
                seg_name = name;
                break;
            }
        }
        if (seg_name.empty())
            continue;

        // Zero-copy: hold shared lock while walking dictionary leaves for this DB.
        // No seg_copy, no temp_tree — serialize directly from arena.
        std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);
        if (!p_entry->is_dirty_.load(std::memory_order_acquire))
            continue;

        const std::string prefix = seg_name + ".";
        for (const auto& [path, id] : t_path_to_id) {
            if (path.compare(0, prefix.size(), prefix) != 0)
                continue;

            auto it = nodes_.find(path);
            if (it == nodes_.end() || !it->second)
                continue;

            // Only emit scalar/array leaves — skip intermediate STRUCT nodes.
            if (!it->second->children_.empty())
                continue;

            const std::string id_str = std::to_string(id);
            writer.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()));
            it->second->serialize(writer, tree(), 0, 0);
        }
        p_entry->getAndClearDirty();
    }

    writer.EndObject();
    return buffer.GetString();
}

void PlcState::clear() {
    nodes_.clear();
    state_storage_.clear();
    top_level_names_.clear();
    tree().clear();
    db_arena_ranges_.clear();
    {
        std::unique_lock<std::shared_mutex> lk(field_index_mutex_);
        field_index_by_db_.clear();
    }
}

void PlcState::incrementNodeVersion(const TreePath& t_path) {
    std::optional<TreePath> current = t_path;
    while (current) {
        auto it = nodes_.find(current->toDotted());
        if (it != nodes_.end() && it->second && it->second->state_) {
            it->second->state_->version_.fetch_add(1, std::memory_order_release);
        }
        current = current->parent();
    }
}

std::vector<TreePath> PlcState::dirtyPathsSince(const TreePath& t_scope, uint64_t t_last_seen_version) const {
    std::vector<TreePath> result;

    auto scope_str = t_scope.toDotted();
    auto it = nodes_.find(scope_str);
    if (it == nodes_.end() || !it->second)
        return result;

    if (!it->second->state_ || it->second->state_->version_.load(std::memory_order_acquire) <= t_last_seen_version) {
        return result; // whole subtree is clean
    }

    auto recurse = [&](auto& t_self, const PlcNode& t_current_node, const TreePath& t_current_path) -> void {
        if (!t_current_node.state_ || t_current_node.state_->version_.load(std::memory_order_acquire) <= t_last_seen_version) {
            return;
        }

        if (t_current_node.children_.empty()) {
            result.push_back(t_current_path);
            return;
        }

        for (const auto& child : t_current_node.children_) {
            std::string child_dotted = t_current_path.empty() ? child.name_ : t_current_path.toDotted() + "." + child.name_;
            auto child_it = nodes_.find(child_dotted);
            if (child_it != nodes_.end() && child_it->second) {
                t_self(t_self, *child_it->second, TreePath::fromDotted(child_dotted));
            }
        }
    };

    recurse(recurse, *it->second, t_scope);
    return result;
}

void PlcState::insertArenaRange(DbEntry* p_entry) {
    // Every DB PlcState registers gets a distinct name, so ArenaTree always
    // allocates a brand-new Segment (see the id-0 "default" in-place-resize
    // special case in ArenaTree::registerSegment, which PlcState never
    // triggers) — a plain sorted insert is safe; there's no existing entry
    // to reconcile.
    ArenaRangeEntry entry{p_entry->offset, p_entry->offset + p_entry->size, p_entry};
    auto it = std::lower_bound(db_arena_ranges_.begin(), db_arena_ranges_.end(), entry.start,
        [](const ArenaRangeEntry& t_r, size_t t_v) { return t_r.start < t_v; });
    db_arena_ranges_.insert(it, entry);
}

PlcState::ArenaLookupResult PlcState::findSegmentByAbsOffset(size_t t_abs_offset, size_t t_size) const {
    auto it = std::upper_bound(db_arena_ranges_.begin(), db_arena_ranges_.end(), t_abs_offset,
        [](size_t t_v, const ArenaRangeEntry& t_r) { return t_v < t_r.start; });
    if (it == db_arena_ranges_.begin())
        return {};
    --it;
    if (t_abs_offset >= it->end)
        return {}; // falls in a gap between segments (padding, or nothing registered there)
    const bool fits = t_size <= it->end - t_abs_offset;
    return {it->p_entry, fits};
}

void PlcState::rebuildFieldIndex() {
    // Pass 1: group leaves by DB and record each one's byte span, using the
    // exact same span rule bumpFieldVersions() used to recompute inline on
    // every single write. Computed once per registration call now.
    ankerl::unordered_dense::map<uint16_t, std::vector<FieldRange>> by_db;
    ankerl::unordered_dense::map<uint16_t, uint32_t> max_span_by_db;

    for (auto& [path, node] : nodes_) {
        if (!node || !node->children_.empty())
            continue; // only leaves are write targets / index entries

        size_t span = node->size_;
        if (node->type_ != s7codec::Type::Struct && node->type_ != s7codec::Type::String && node->type_ != s7codec::Type::WString) {
            span = static_cast<size_t>(s7codec::primitiveSize(node->type_).value_or(0)) * std::max(1u, node->count_);
        }
        if (span == 0)
            continue;

        by_db[node->db_number_].push_back({node->offset_, static_cast<uint32_t>(node->offset_ + span), node.get()});
        auto& mx = max_span_by_db[node->db_number_];
        mx = std::max<uint32_t>(mx, static_cast<uint32_t>(span));
    }

    // Pass 2: wire up parent_ pointers for bumpVersionChain(). Derived from
    // full_path_ (set by add() for every stored node) — one extra hash
    // lookup per node, paid once here, never on the hot path.
    for (auto& [path, node] : nodes_) {
        if (!node)
            continue;
        node->parent_ = nullptr;
        auto pos = path.find_last_of('.');
        if (pos == std::string::npos)
            continue;
        auto pit = nodes_.find(path.substr(0, pos));
        if (pit != nodes_.end() && pit->second)
            node->parent_ = pit->second.get();
    }

    std::unique_lock<std::shared_mutex> lk(field_index_mutex_);
    field_index_by_db_.clear();
    for (auto& [db, vec] : by_db) {
        std::sort(vec.begin(), vec.end(), [](const FieldRange& t_a, const FieldRange& t_b) { return t_a.start < t_b.start; });
        auto idx = std::make_shared<DbFieldIndex>();
        idx->ranges = std::move(vec);
        idx->max_leaf_span = max_span_by_db[db];
        field_index_by_db_[db] = std::move(idx);
    }
}

} // namespace sgrn::gateway::twin
