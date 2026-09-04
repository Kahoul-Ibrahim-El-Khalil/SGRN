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
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <map>
#include <memory>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <s7codec/types.hpp>
#include <shared_mutex>
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
// PlcNodeState — lock-free per-leaf state (split from descriptor)
// ---------------------------------------------------------------------------
struct PlcNodeState {
    std::atomic<uint64_t> version_{0};
    mutable std::atomic<bool> field_dirty_{false};

    PlcNodeState() = default;
    PlcNodeState(const PlcNodeState& o)
        : version_(o.version_.load(std::memory_order_relaxed))
        , field_dirty_(o.field_dirty_.load(std::memory_order_relaxed)) {
    }
    PlcNodeState& operator=(const PlcNodeState& o) {
        version_.store(o.version_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        field_dirty_.store(o.field_dirty_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

// ---------------------------------------------------------------------------
// PlcNode — replaces sgrn::Symbol
// ---------------------------------------------------------------------------

/**
 * @brief Represents a symbolic PLC variable in the digital twin state.
 *
 * Replaces sgrn::Symbol. Nodes live in PlcState::nodes_ and refer to
 * their physical memory via cached_slot (a pointer into PlcArena::DbEntry).
 * Descriptor fields are copyable; state (version/dirty) lives in PlcNodeState
 * owned by PlcState and pointed to via state_ for lock-free O(depth) bumps.
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

    PlcNode* parent_{nullptr};

    // Split state: owned by PlcState::state_storage_, pointed to here for
    // O(depth) bumpVersionChain without hash lookup. Children in the
    // descriptor tree (children_) do not have state_ (only flat nodes_ do).
    PlcNodeState* state_{nullptr};

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
        , parent_(t_other.parent_)
        , state_(t_other.state_) {
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
        , parent_(t_other.parent_)
        , state_(t_other.state_) {
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
            parent_ = t_other.parent_;
            state_ = t_other.state_;
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
            parent_ = t_other.parent_;
            state_ = t_other.state_;
        }
        return *this;
    }
    bool is_dirty() const { // REVAMP-4: Derived from parent DB
        return cached_slot_ && cached_slot_->is_dirty_.load(std::memory_order_acquire);
    }
    void bumpVersionChain() {
        for (PlcNode* p = this; p; p = p->parent_) {
            if (p->state_)
                p->state_->version_.fetch_add(1, std::memory_order_release);
        }
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
class PlcState {
public:
    PlcState();

    // ── Arena (has-a, was base Registry) ─────────────────────────────────────
    ::sgrn::ArenaTree& tree() {
        return arena_;
    }
    const ::sgrn::ArenaTree& tree() const {
        return arena_;
    }

    // ── DB Registration ───────────────────────────────────────────────────────

    /**
     * @brief Add a PlcNode (DB or field) to the state store.
     */
    void add(PlcNode t_node, const std::string& t_parent_path = "", std::optional<size_t> t_fixed_offset = std::nullopt);

    // ── Accessors ─────────────────────────────────────────────────────────────

    void registerSegment(const std::string& t_name, uint16_t t_id, size_t t_size_bytes) {
        tree().registerSegment(t_name, t_id, t_size_bytes);
        if (DbEntry* p_entry = findSegmentById(t_id))
            insertArenaRange(p_entry);
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

    static inline constexpr std::array<uint8_t, 256> k_uppercase_table = []() {
        std::array<uint8_t, 256> table{};
        for (size_t i = 0; i < 256; ++i) {
            table[i] = (i >= 'a' && i <= 'z') ? static_cast<uint8_t>(i - 'a' + 'A') : static_cast<uint8_t>(i);
        }
        return table;
    }();

    struct CaseInsensitiveHash {
        size_t operator()(const std::string& t_s) const noexcept {
            size_t h = 14695981039346656037ULL;
            for (char c : t_s) {
                const uint8_t uc = k_uppercase_table[static_cast<uint8_t>(c)];
                h = (h ^ uc) * 1099511628211ULL;
            }
            return h;
        }
    };
    struct CaseInsensitiveEqual {
        bool operator()(const std::string& t_a, const std::string& t_b) const noexcept {
            if (t_a.size() != t_b.size())
                return false;
            for (size_t i = 0; i < t_a.size(); ++i) {
                if (k_uppercase_table[static_cast<uint8_t>(t_a[i])] != k_uppercase_table[static_cast<uint8_t>(t_b[i])])
                    return false;
            }
            return true;
        }
    };

    const PlcNode* find(const std::string& t_path) const; // REVAMP-10: O(1) hashed lookup

    using NodeMap = ankerl::unordered_dense::map<std::string, std::unique_ptr<PlcNode>, CaseInsensitiveHash, CaseInsensitiveEqual>;
    const NodeMap& nodes() const {
        return nodes_;
    }

    // ── REVAMP-PERF: precomputed indexes (schema-registration time) ────────────
    //
    // Both indexes below are rebuilt from scratch by rebuildFieldIndex(),
    // which must be called once after a registration pass finishes adding
    // nodes (PlcMemory::loadRegistry()/registerDb() do this for you — see
    // their end). They are NOT maintained incrementally, because nodes_ is
    // an ankerl::unordered_dense::map: it invalidates PlcNode* on rehash,
    // so any pointer cached mid-registration could dangle by the time the
    // next node is inserted. Rebuilding once at the end of each
    // registration call sidesteps that: every pointer is re-derived from
    // nodes_ as it stands at that moment, and nothing mutates nodes_ again
    // until the next registration call triggers another full rebuild.

    struct FieldRange {
        uint32_t start; // inclusive, DB-relative byte offset
        uint32_t end;   // exclusive
        PlcNode* p_node;
    };
    struct DbFieldIndex {
        std::vector<FieldRange> ranges; // sorted by start
        uint32_t max_leaf_span{0};      // longest single leaf's byte span in this DB
    };

    /// Rebuild the per-DB leaf-range index and every leaf's parent_ pointer
    /// from the current nodes_ contents. O(N log N) once per registration
    /// call — never on the read/write hot path.
    void rebuildFieldIndex();

    /// Call t_fn(PlcNode&) for every leaf of t_db_number whose byte range
    /// intersects [t_offset, t_offset + t_size). O(log L + k + w): L =
    /// leaves in that DB, k = leaves actually touched, w = a bounded
    /// backward-scan window (see rebuildFieldIndex.cpp comment) needed to
    /// correctly find overlapping ranges from aliased fields. Allocation-free.
    template <typename Fn>
    void forEachIntersectingLeaf(uint16_t t_db_number, size_t t_offset, size_t t_size, Fn&& t_fn) const {
        std::shared_ptr<const DbFieldIndex> idx;
        {
            std::shared_lock<std::shared_mutex> lk(field_index_mutex_);
            auto it = field_index_by_db_.find(t_db_number);
            if (it == field_index_by_db_.end())
                return;
            idx = it->second; // cheap refcount bump; lock released right after
        }
        if (!idx || idx->ranges.empty())
            return;

        const auto& ranges = idx->ranges;
        const size_t write_end = t_offset + t_size;

        // First entry with start >= write_end; everything before it has start < write_end.
        auto hi_it =
            std::upper_bound(ranges.begin(), ranges.end(), write_end, [](size_t t_v, const FieldRange& t_r) { return t_v <= t_r.start; });

        for (auto it = hi_it; it != ranges.begin();) {
            --it;
            if (it->end <= t_offset) {
                // This range ends before our window starts. Ranges are sorted
                // by start, not end, so an earlier-starting range COULD still
                // reach into the window (an alias spanning several fields) --
                // we only stop once it's provably impossible: no leaf in this
                // DB is longer than max_leaf_span, so once even the longest
                // possible leaf starting at/before `it->start` couldn't reach
                // t_offset, nothing earlier can either.
                if (static_cast<size_t>(it->start) + idx->max_leaf_span <= t_offset)
                    break;
                continue;
            }
            t_fn(*it->p_node);
        }
    }

    /// Call t_fn(PlcNode&) for every leaf of t_db_number, offset order.
    /// O(L). Replaces a global nodes_ scan filtered by string prefix.
    template <typename Fn>
    void forEachLeaf(uint16_t t_db_number, Fn&& t_fn) const {
        std::shared_ptr<const DbFieldIndex> idx;
        {
            std::shared_lock<std::shared_mutex> lk(field_index_mutex_);
            auto it = field_index_by_db_.find(t_db_number);
            if (it == field_index_by_db_.end())
                return;
            idx = it->second;
        }
        if (!idx)
            return;
        for (auto& r : idx->ranges)
            t_fn(*r.p_node);
    }

    /// O(log D) lookup of the DB segment containing [t_abs_offset, t_abs_offset+t_size),
    /// D = number of registered DBs. Replaces a linear scan over segments().
    struct ArenaLookupResult {
        DbEntry* p_entry{nullptr};
        bool fits{false}; // false + non-null p_entry means t_size runs past the segment end
    };
    ArenaLookupResult findSegmentByAbsOffset(size_t t_abs_offset, size_t t_size) const;

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
     * Produces a nested JSON: {"ReactorCore": {"field": value, ...}}
     */
    std::string getDeltaSnapshot(const std::vector<uint16_t>& t_filter = {}) const;

    /**
     * @brief Flat numeric-keyed delta snapshot (dictionary / Phase-4 mode).
     *
     * Like getDeltaSnapshot() but uses @p t_path_to_id to emit leaf IDs as
     * keys instead of nested DB-object names. Produces {"<id>": value, ...}.
     * Only leaves whose path exists in t_path_to_id are emitted; leaves not
     * present in the dictionary (e.g. STRUCT nodes) are skipped silently.
     * Dirty flags are cleared identically to getDeltaSnapshot().
     */
    std::string getDeltaSnapshotFlat(
        const ankerl::unordered_dense::map<std::string, uint32_t>& t_path_to_id, const std::vector<uint16_t>& t_filter = {}) const;

    // ── Cache control ─────────────────────────────────────────────────────────
    void setCacheEnabled(bool t_enabled) {
        cache_enabled_ = t_enabled;
    }
    bool isCacheEnabled() const {
        return cache_enabled_;
    }

    const std::vector<std::string>& getTopLevelNames() const {
        return top_level_names_;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void clear();

private:
    ::sgrn::ArenaTree arena_;
    std::vector<std::string> top_level_names_;
    bool cache_enabled_{true};
    ankerl::unordered_dense::map<std::string, std::unique_ptr<PlcNode>, CaseInsensitiveHash, CaseInsensitiveEqual> nodes_;
    // Split state storage: descriptor (PlcNode) is trivially copyable, state
    // (version/dirty) is heap-allocated and pointed to via PlcNode::state_.
    // Stable across map rehash (unique_ptr nodes) and across descriptor copies.
    std::vector<std::unique_ptr<PlcNodeState>> state_storage_;

    // MPSC Queue using mutex for now (low contention, fast)
    mutable std::mutex command_mutex_;
    std::vector<PlcCommand> commands_;

    // REVAMP-PERF: arena-range index for findSegmentByAbsOffset(). DbEntry*
    // is unique_ptr-owned inside ArenaTree::segments_ (see ArenaTree.hpp) and
    // is never moved/reallocated once created, so caching raw pointers here
    // is safe even while more DBs are still being registered elsewhere.
    // NOTE (inherited assumption, not new): like the scan it replaces, this
    // relies on schema registration completing before read/write traffic
    // starts — registerSegment() isn't synchronized against concurrent
    // findSegmentByAbsOffset() callers. If DBs can be registered at runtime
    // while traffic is already flowing, this needs its own mutex.
    struct ArenaRangeEntry {
        size_t start;
        size_t end; // exclusive
        DbEntry* p_entry;
    };
    std::vector<ArenaRangeEntry> db_arena_ranges_; // kept sorted by start
    void insertArenaRange(DbEntry* p_entry);

    // REVAMP-PERF: string pool for symbol deduplication
    std::shared_ptr<const std::string> internString(const std::string& t_str) {
        auto it = string_pool_.find(t_str);
        if (it != string_pool_.end()) {
            return it->second;
        }
        auto sp = std::make_shared<const std::string>(t_str);
        string_pool_[t_str] = sp;
        return sp;
    }

    mutable std::shared_mutex field_index_mutex_;
    ankerl::unordered_dense::map<uint16_t, std::shared_ptr<const DbFieldIndex>> field_index_by_db_;
    ankerl::unordered_dense::map<std::string, std::shared_ptr<const std::string>> string_pool_;
};

} // namespace sgrn::gateway::twin
