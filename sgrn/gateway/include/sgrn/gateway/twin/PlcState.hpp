#pragma once
// REVAMP-1: New file — PlcState.hpp replaces sgrn/core Registry.hpp for s7 use.
// Lives in sgrn::gateway::twin namespace. sgrn::Registry kept for sgrn/opc consumers.

#include <sgrn/gateway/twin/PlcCommand.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <sgrn/structures/ArenaTree.hpp>
#include <sgrn/structures/Registry.hpp>
#include <sgrn/types/UniversalType.hpp>
#include <sgrn/utils/endianess.hpp>
#include <sgrn/utils/time.hpp>
#include <map>
#include <memory>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <s7codec/types.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::twin
{
class PlcState;

using DbEntry = ::sgrn::ArenaTree::Segment;
using LockMode = ::sgrn::ArenaTree::LockMode;
using ScopedLock = ::sgrn::ArenaTree::SegmentLock;

// ---------------------------------------------------------------------------
// PlcNode — replaces sgrn::Symbol
// ---------------------------------------------------------------------------

/**
 * @brief Represents a symbolic PLC variable in the digital twin state.
 *
 * Replaces sgrn::Symbol. Nodes live in PlcState::nodes_ and refer to
 * their physical memory via cached_slot (a pointer into PlcArena::DbEntry).
 */
struct PlcNode {
    std::string name_;
    sgrn::UniversalType universal_type_{sgrn::UniversalType::Unknown};
    size_t size_{0};
    uint16_t id_{0};
    // Optimized Context (replaces Json::Value)
    uint32_t offset_{0};
    uint32_t count_{1};
    uint32_t string_capacity_{0};
    std::optional<double> min_val_{std::nullopt};
    std::optional<double> max_val_{std::nullopt};

    uint16_t db_number_{0};
    uint8_t bit_index_{0};
    s7codec::Type type_{s7codec::Type::Byte};
    s7codec::Endian endian_{s7codec::Endian::Big};
    bool is_dynamic_{false};
    /// Numeric→symbolic mapping for enum-typed fields (from scl::DbField::enum_map).
    /// Used by the JSON read paths so enums round-trip as symbolic strings.
    std::map<int, std::string> enum_map_;
    std::vector<PlcNode> children_;
    std::string full_path_; // Store for collision verification in find()
    // PlcNode struct — add after string_capacity_
    // Cache — points into PlcArena (stable for arena lifetime)
    mutable const DbEntry* cached_slot_{nullptr}; // REVAMP-3: DbEntry* replaces LeafDescriptor*

    // Tier 1: Version tracking
    std::atomic<uint64_t> version_{0};

    // Field-level dirty tracking: set by bumpFieldVersions() when this
    // leaf's byte range overlaps a write and the data actually changed.
    // Cleared by collectTypedDirtyLeaves() after the change is delivered.
    mutable std::atomic<bool> field_dirty_{false};

    PlcNode() = default;

    PlcNode(const PlcNode& t_other)
        : name_(t_other.name_)
        , universal_type_(t_other.universal_type_)
        , size_(t_other.size_)
        , id_(t_other.id_)
        , offset_(t_other.offset_)
        , count_(t_other.count_)
        , string_capacity_(t_other.string_capacity_)
        , db_number_(t_other.db_number_)
        , bit_index_(t_other.bit_index_)
        , type_(t_other.type_)
        , endian_(t_other.endian_)
        , is_dynamic_(t_other.is_dynamic_)
        , enum_map_(t_other.enum_map_)
        , children_(t_other.children_)
        , full_path_(t_other.full_path_)
        , cached_slot_(t_other.cached_slot_)
        , version_(t_other.version_.load(std::memory_order_relaxed))
        , field_dirty_(t_other.field_dirty_.load(std::memory_order_relaxed)) {
    }

    PlcNode(PlcNode&& t_other) noexcept
        : name_(std::move(t_other.name_))
        , universal_type_(t_other.universal_type_)
        , size_(t_other.size_)
        , id_(t_other.id_)
        , offset_(t_other.offset_)
        , count_(t_other.count_)
        , string_capacity_(t_other.string_capacity_)
        , db_number_(t_other.db_number_)
        , bit_index_(t_other.bit_index_)
        , type_(t_other.type_)
        , endian_(t_other.endian_)
        , is_dynamic_(t_other.is_dynamic_)
        , enum_map_(t_other.enum_map_)
        , children_(std::move(t_other.children_))
        , full_path_(std::move(t_other.full_path_))
        , cached_slot_(t_other.cached_slot_)
        , version_(t_other.version_.load(std::memory_order_relaxed))
        , field_dirty_(t_other.field_dirty_.load(std::memory_order_relaxed)) {
    }

    PlcNode& operator=(PlcNode&& t_other) noexcept {
        if (this != &t_other) {
            name_ = std::move(t_other.name_);
            universal_type_ = t_other.universal_type_;
            size_ = t_other.size_;
            id_ = t_other.id_;
            offset_ = t_other.offset_;
            count_ = t_other.count_;
            string_capacity_ = t_other.string_capacity_;
            db_number_ = t_other.db_number_;
            bit_index_ = t_other.bit_index_;
            type_ = t_other.type_;
            endian_ = t_other.endian_;
            is_dynamic_ = t_other.is_dynamic_;
            enum_map_ = t_other.enum_map_;
            children_ = std::move(t_other.children_);
            full_path_ = std::move(t_other.full_path_);
            cached_slot_ = t_other.cached_slot_;
            version_.store(t_other.version_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            field_dirty_.store(t_other.field_dirty_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    PlcNode& operator=(const PlcNode& t_other) {
        if (this != &t_other) {
            name_ = t_other.name_;
            universal_type_ = t_other.universal_type_;
            size_ = t_other.size_;
            id_ = t_other.id_;
            offset_ = t_other.offset_;
            count_ = t_other.count_;
            string_capacity_ = t_other.string_capacity_;
            db_number_ = t_other.db_number_;
            bit_index_ = t_other.bit_index_;
            type_ = t_other.type_;
            endian_ = t_other.endian_;
            is_dynamic_ = t_other.is_dynamic_;
            enum_map_ = t_other.enum_map_;
            children_ = t_other.children_;
            full_path_ = t_other.full_path_;
            cached_slot_ = t_other.cached_slot_;
            version_.store(t_other.version_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            field_dirty_.store(t_other.field_dirty_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    bool is_dirty() const { // REVAMP-4: Derived from parent DB
        return cached_slot_ && cached_slot_->is_dirty_.load(std::memory_order_acquire);
    }

    /**
     * @brief Recursive serialization using RapidJSON Writer.
     *
     * SEMANTIC NESTING & SUBTREE EXTRACTION
     * This method provides "First-Class Nesting" by recursively projecting
     * flat arena bytes into a structured JSON tree.
     *   - Depth over Breadth: Handles deeply nested UDTs/Structs natively.
     *   - Granular Access: Allows extracting any sub-path (e.g. Mixer.Pump.Motor)
     *     without serializing the entire memory block.
     */
    template <typename Writer>
    void serialize(Writer& t_writer, const ::sgrn::ArenaTree& t_arena, int t_depth = 0,
        size_t t_extra_offset = 0) const { // REVAMP-5
        if (!cached_slot_) {
            t_writer.Null();
            return;
        }

        // The absolute offset of this node (or the first element if array)
        // is its rel_offset + any offset inherited from parents (extra_offset).
        size_t base_abs_offset = t_extra_offset + offset_;

        int local_count = static_cast<int>(count_);
        if (local_count < 1)
            local_count = 1;

        if (is_dynamic_) {
            // Read 4-byte current count header
            const uint8_t* p_head_ptr = t_arena.data() + cached_slot_->offset + base_abs_offset;
            int current_count = static_cast<int>(s7codec::fromEndian<uint32_t>(p_head_ptr, endian_));
            local_count = std::min(local_count, current_count);
            base_abs_offset += 4; // Advance past the header
        }

        bool is_s7_string = (type_ == s7codec::Type::String || type_ == s7codec::Type::WString || type_ == s7codec::Type::XString ||
                             type_ == s7codec::Type::XWString);

        // After the offset-tracker fix:
        //   - scalar string:  count_=1,  size_=per-element byte span, string_capacity_=chars
        //   - string array:   count_=N,  size_=per-element byte span, string_capacity_=chars
        //   - other arrays:   count_=N,  size_=per-element byte span
        const bool is_string_array = (is_s7_string && local_count > 1);
        bool is_array = (local_count > 1 && (!is_s7_string || is_string_array));

        size_t elem_size = this->size_;
        // For non-array strings, the entire 'size' is the element size.
        // For arrays, 'size' should be the element size.
        // In loadRegistry, n.size for strings is typeSpanBytes(char_capacity), which is the per-element span.
        if (is_array)
            t_writer.StartArray();

        for (int i = 0; i < (is_array ? local_count : 1); ++i) {
            // Offset for the i-th element of the array
            size_t current_abs_offset = base_abs_offset;
            int b_idx = static_cast<int>(bit_index_);

            if (is_array) {
                if (type_ != s7codec::Type::Bool) {
                    current_abs_offset += (i * elem_size);
                }
            }

            // Bounds check against arena size to prevent Segfaults
            size_t check_size =
                children_.empty() ? (type_ == s7codec::Type::Bool ? ((is_array ? (local_count + 7) / 8 : 1)) : elem_size) : 0;
            if (current_abs_offset + check_size > cached_slot_->offset + cached_slot_->size) {
                if (children_.empty())
                    t_writer.Null();
                else {
                    t_writer.StartObject();
                    t_writer.EndObject();
                }
                if (!is_array)
                    break;
                continue;
            }

            if (children_.empty()) {
                // --- Scalar Logic ---
                const uint8_t* p_ptr = t_arena.data() + cached_slot_->offset + current_abs_offset;
                size_t buffer_remaining = (cached_slot_->offset + cached_slot_->size) - (cached_slot_->offset + current_abs_offset);

                // For string array elements: decode_count = char capacity.
                // For scalar strings: decode_count = string_capacity_ (or count_ for legacy).
                // For non-string arrays: decode_count = 1 (one element at a time).
                // For Bool arrays: adjusted below.
                int decode_count;
                if (is_string_array) {
                    decode_count = static_cast<int>(string_capacity_ > 0 ? string_capacity_ : (size_ > 2 ? size_ - 2 : 0));
                } else if (is_s7_string) {
                    decode_count = static_cast<int>(string_capacity_ > 0 ? string_capacity_ : count_);
                } else {
                    decode_count = is_array ? 1 : static_cast<int>(count_);
                }

                // If it's a bool array, we adjust the pointer and bit index manually to match S7 packing
                if (is_array && type_ == s7codec::Type::Bool) {
                    p_ptr = t_arena.data() + cached_slot_->offset + base_abs_offset + (i / 8);
                    b_idx = (i % 8);
                    buffer_remaining = (cached_slot_->offset + cached_slot_->size) - (cached_slot_->offset + base_abs_offset + (i / 8));
                }

                auto dv = s7codec::decodeScalar(type_, p_ptr, buffer_remaining, b_idx, decode_count, endian_);
                if (!dv.valid()) {
                    t_writer.Null();
                } else {
                    // Enum fields round-trip as their symbolic name instead of a
                    // bare number (scalar-only; arrays stay numeric).
                    if (!enum_map_.empty() && !is_array) {
                        const int64_t numeric = (dv.kind() == s7codec::ValueKind::UnsignedInt) ? static_cast<int64_t>(dv.u())
                                                : (dv.kind() == s7codec::ValueKind::SignedInt) ? dv.i()
                                                                                               : INT64_MIN;
                        if (numeric != INT64_MIN) {
                            auto it = enum_map_.find(static_cast<int>(numeric));
                            if (it != enum_map_.end()) {
                                t_writer.String(it->second.c_str(), static_cast<rapidjson::SizeType>(it->second.length()));
                            } else {
                                // Unknown numeric value — fall back to the number.
                                switch (dv.kind()) {
                                    case s7codec::ValueKind::SignedInt:
                                        t_writer.Int64(dv.i());
                                        break;
                                    case s7codec::ValueKind::UnsignedInt:
                                        t_writer.Uint64(dv.u());
                                        break;
                                    default:
                                        t_writer.Null();
                                        break;
                                }
                            }
                            if (!is_array)
                                break;
                            continue;
                        }
                    }
                    // AFTER
                    switch (dv.kind()) {
                        case s7codec::ValueKind::Bool:
                            t_writer.Bool(dv.b());
                            break;
                        case s7codec::ValueKind::SignedInt:
                            t_writer.Int64(dv.i());
                            break;
                        case s7codec::ValueKind::UnsignedInt:
                            t_writer.Uint64(dv.u());
                            break;
                        case s7codec::ValueKind::Float:
                            t_writer.Double(static_cast<double>(dv.f()));
                            break;
                        case s7codec::ValueKind::Double:
                            t_writer.Double(dv.d());
                            break;
                        case s7codec::ValueKind::String:
                            t_writer.String(dv.s().c_str(), static_cast<rapidjson::SizeType>(dv.s().length()));
                            break;
                        default:
                            t_writer.Null();
                            break;
                    }
                }
            } else {
                // --- Struct Logic ---
                t_writer.StartObject();
                for (const auto& child : children_) {
                    t_writer.Key(child.name_.c_str(), static_cast<rapidjson::SizeType>(child.name_.length()));
                    // Recurse: pass current_abs_offset as the base for the child
                    child.serialize(t_writer, t_arena, t_depth + 1, current_abs_offset);
                }
                t_writer.EndObject();
            }

            if (!is_array)
                break;
        }

        if (is_array)
            t_writer.EndArray();
    }
};

/**
 * @brief Unified Digital Twin state for the S7 gateway.
 *
 * Replaces sgrn::Registry. Owns a PlcArena (flat byte buffer) and a map
 * of PlcNode trees that describe the semantic layout of each DB.
 */
/**
 * @brief Core Semantic Data Model mapping symbolic paths to raw memory.
 *
 * Architectural Relationship:
 * ───────────────────────────
 * PlcState holds the semantic tree of the entire PLC configuration
 * (e.g., ReactorCore/speed). It maintains `PlcNode` objects which
 * map string paths to byte offsets in the `PlcArena`.
 *
 * - PlcArena (Base class Registry): Provides the raw sequential byte buffer.
 * - PlcMemory: The OT bridge. Updates the bytes in PlcArena, and uses
 *   PlcState to know where those bytes map semantically.
 * - TreeCacheEngine: Uses PlcState to lookup nodes by path to generate JSON.
 * - TelemetryBroker: PlcState generates `DeltaSnapshot` JSON for the broker.
 */
class PlcState : public ::sgrn::Registry {
public:
    PlcState();

    // ── DB Registration ───────────────────────────────────────────────────────

    /**
     * @brief Add a PlcNode (DB or field) to the state store.
     */
    void add(PlcNode t_node, const std::string& t_parent_path = "", std::optional<size_t> t_fixed_offset = std::nullopt);

    // ── Accessors ─────────────────────────────────────────────────────────────

    void registerSegment(const std::string& t_name, uint16_t t_id, size_t t_size_bytes) {
        tree().registerSegment(t_name, t_id, t_size_bytes);
    }
    DbEntry* findSegmentById(uint16_t t_id) {
        auto it = tree().segments_by_id().find(t_id);
        return it != tree().segments_by_id().end() ? it->second : nullptr;
    }
    DbEntry* findSegmentByName(const std::string& t_name) {
        auto it = tree().segments().find(t_name);
        return it != tree().segments().end() ? it->second.get() : nullptr;
    }
    const std::map<std::string, std::unique_ptr<DbEntry>>& segments() const {
        return tree().segments();
    }
    auto lockSegments(const std::vector<uint32_t>& t_ids, LockMode t_mode) {
        return tree().lockSegments(t_ids, t_mode);
    }
    uint8_t* arenaData() {
        return tree().data();
    }
    const uint8_t* arenaData() const {
        return tree().data();
    }
    size_t arenaSize() const {
        return tree().size();
    }
    ::sgrn::ArenaTree& getArenaTree() {
        return tree();
    }
    const ::sgrn::ArenaTree& getArenaTree() const {
        return tree();
    }

    struct CaseInsensitiveHash {
        size_t operator()(const std::string& t_s) const {
            size_t h = 0;
            for (char c : t_s)
                h = h * 31 + static_cast<size_t>(std::toupper(static_cast<unsigned char>(c)));
            return h;
        }
    };
    struct CaseInsensitiveEqual {
        bool operator()(const std::string& t_a, const std::string& t_b) const {
            return t_a.size() == t_b.size() && std::equal(t_a.begin(), t_a.end(), t_b.begin(), [](char t_x, char t_y) {
                return std::toupper(static_cast<unsigned char>(t_x)) == std::toupper(static_cast<unsigned char>(t_y));
            });
        }
    };

    const PlcNode* find(const std::string& t_path) const; // REVAMP-10: O(1) hashed lookup

    using NodeMap = std::unordered_map<std::string, PlcNode, CaseInsensitiveHash, CaseInsensitiveEqual>;
    const NodeMap& nodes() const {
        return nodes_;
    }

    // ── Versioning (Tier 1) ───────────────────────────────────────────────────

    void incrementNodeVersion(const TreePath& t_path);
    std::vector<TreePath> dirtyPathsSince(const TreePath& t_scope, uint64_t t_last_seen_version) const;

    std::string getJsonString(const std::string& t_path) const;

    /**
     * @brief Fast scalar value retrieval — avoids full RapidJSON writer pipeline.
     * Falls back to getJsonString() for non-scalar nodes.
     */
    std::string getScalarString(const std::string& t_path) const;

    // ── Command Queue (MPSC) ──────────────────────────────────────────────────

    void pushCommand(PlcCommand t_cmd);      // REVAMP-11
    std::vector<PlcCommand> drainCommands(); // REVAMP-12
    bool hasPendingCommands() const;

    // ── Top-level names (for Northbound iteration) ────────────────────────────

    std::vector<uint16_t> topLevelNumbers() const;

    std::vector<std::string> names() const;

    // ── Serialization ─────────────────────────────────────────────────────────

    std::string toJson() const;
    void loadFromJson(const std::string& t_json_payload);

    /**
     * @brief Full snapshot — serializes all top-level DBs, locks shared.
     */
    std::string getFullSnapshot() const;

    /**
     * @brief Flat, leaf-id-keyed snapshot (dictionary mode)
     */
    std::string getFullSnapshotFlat(
        const std::unordered_map<std::string, uint32_t>& t_path_to_id, const std::vector<bool>& t_allowed_by_id = {}) const;

    /**
     * @brief Delta snapshot — serializes only dirty DB ranges, clears dirty marks.
     */
    std::string getDeltaSnapshot(const std::vector<uint16_t>& t_filter = {}) const;

    // ── Cache control ─────────────────────────────────────────────────────────

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void clear();

private:
    // PlcArena arena_; // Removed: using base Registry::tree()
    std::unordered_map<std::string, PlcNode, CaseInsensitiveHash, CaseInsensitiveEqual> nodes_;

    // MPSC Queue using mutex for now (low contention, fast)
    mutable std::mutex command_mutex_;
    std::vector<PlcCommand> commands_;
};

} // namespace sgrn::gateway::twin
