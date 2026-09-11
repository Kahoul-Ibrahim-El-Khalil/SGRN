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
#include <stdexcept>
#include <string>

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

namespace
{

/// Case-insensitive string comparison for BOOL/enum initializer literals.
bool initEqualsIgnoreCase(const std::string& t_a, const std::string& t_b) {
    if (t_a.size() != t_b.size())
        return false;

    for (size_t i = 0; i < t_a.size(); ++i) {
        const char ca = (t_a[i] >= 'A' && t_a[i] <= 'Z') ? static_cast<char>(t_a[i] + ('a' - 'A')) : t_a[i];
        const char cb = (t_b[i] >= 'A' && t_b[i] <= 'Z') ? static_cast<char>(t_b[i] + ('a' - 'A')) : t_b[i];

        if (ca != cb)
            return false;
    }

    return true;
}

std::string trimInitValue(const std::string& t_s) {
    const size_t b = t_s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};

    const size_t e = t_s.find_last_not_of(" \t\r\n");
    return t_s.substr(b, e - b + 1);
}

/// Seeds a single DB field's initializer (`:= <value>`) into freshly allocated
/// DB memory. Recurses into STRUCT children so nested initializers are applied.
/// Arrays are intentionally skipped (single-scalar initializers only today).
void applyFieldInit(PlcMemory& t_mem, uint16_t t_db, const scl::DbField& t_field) {
    if (t_field.init_value.empty())
        return;

    if (t_field.type == scl::DataType::Struct) {
        for (const auto& child : t_field.children)
            applyFieldInit(t_mem, t_db, child);
        return;
    }

    const bool is_string = t_field.type == scl::DataType::String || t_field.type == scl::DataType::WString ||
                           t_field.type == scl::DataType::XString || t_field.type == scl::DataType::XWString;

    const bool is_array = !is_string && t_field.count > 1;
    if (is_array)
        return; // array initializers (TIA `[a, b, c]`) are not supported yet

    std::string raw = trimInitValue(t_field.init_value);
    if (raw.empty())
        return;

    const int max_len = is_string ? (t_field.struct_size > 0 ? t_field.struct_size : t_field.count) : 1;

    const size_t span = static_cast<size_t>(s7codec::typeSpanBytes(t_field.type, is_string ? max_len : 1).value_or(0));

    if (span == 0)
        return;

    std::vector<uint8_t> buf(span, 0);
    if (buf.empty())
        return;

    // Quoted string literal ("Siemens" or 'Siemens') → native string encode.
    if (is_string) {
        if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') || (raw.front() == '\'' && raw.back() == '\''))) {
            raw = raw.substr(1, raw.size() - 2);
        }

        auto dv = s7codec::DecodedValue::makeString(std::move(raw));
        auto st = s7codec::encodeScalar(dv, t_field.type, buf.data(), buf.size(), 0, max_len, t_field.endianness);

        if (st.has_value())
            (void)t_mem.writeDbMemory(t_db, static_cast<size_t>(t_field.offset), buf.size(), buf.data());

        return;
    }

    // Enum literal: resolve a matching name to its numeric value first.
    if (!t_field.enum_map.empty()) {
        for (const auto& [k, v] : t_field.enum_map) {
            if (initEqualsIgnoreCase(v, raw)) {
                auto dv = s7codec::DecodedValue::makeSigned(k);
                auto st = s7codec::encodeScalar(dv, t_field.type, buf.data(), buf.size(), t_field.bit_index, 1, t_field.endianness);

                if (st.has_value())
                    (void)t_mem.writeDbMemory(t_db, static_cast<size_t>(t_field.offset), buf.size(), buf.data());

                return;
            }
        }
    }

    // BOOL literal.
    if (t_field.type == scl::DataType::Bool) {
        bool b = false;

        if (initEqualsIgnoreCase(raw, "true") || raw == "1")
            b = true;
        else if (!initEqualsIgnoreCase(raw, "false") && raw != "0")
            return;

        auto dv = s7codec::DecodedValue::makeSigned(b ? 1 : 0);
        auto st = s7codec::encodeScalar(dv, scl::DataType::Bool, buf.data(), buf.size(), t_field.bit_index, 1, t_field.endianness);

        if (st.has_value())
            (void)t_mem.writeDbMemory(t_db, static_cast<size_t>(t_field.offset), buf.size(), buf.data());

        return;
    }

    // Numeric literal. Rejoin sign/number tokens ("- 3" → "-3") for ease of use.
    std::string num;
    num.reserve(raw.size());

    for (char c : raw) {
        if (c != ' ')
            num += c;
    }

    if (num.empty())
        return;

    if (t_field.type == scl::DataType::Real || t_field.type == scl::DataType::LReal) {

        char* end = nullptr;
        const double d = std::strtod(num.c_str(), &end);

        if (!end || *end != '\0')
            return;

        auto dv = s7codec::DecodedValue::makeDouble(d);
        auto st = s7codec::encodeScalar(dv, t_field.type, buf.data(), buf.size(), 0, 1, t_field.endianness);

        if (st.has_value())
            (void)t_mem.writeDbMemory(t_db, static_cast<size_t>(t_field.offset), buf.size(), buf.data());

        return;
    }

    char* end = nullptr;
    const long long v = std::strtoll(num.c_str(), &end, 0);

    if (!end || *end != '\0')
        return;

    auto dv = s7codec::DecodedValue::makeSigned(v);
    auto st = s7codec::encodeScalar(dv, t_field.type, buf.data(), buf.size(), t_field.bit_index, 1, t_field.endianness);

    if (st.has_value())
        (void)t_mem.writeDbMemory(t_db, static_cast<size_t>(t_field.offset), buf.size(), buf.data());
}

/// Applies `:=` initializers for a whole DB's field tree.
void applyDbFieldInits(PlcMemory& t_mem, uint16_t t_db, const std::vector<scl::DbField>& t_fields) {

    for (const auto& f : t_fields)
        applyFieldInit(t_mem, t_db, f);
}

} // namespace

PlcMemory::PlcMemory()
    : cmd_processor_(std::make_unique<PlcCommandProcessor>(*this))
    , snapshot_registry_(std::make_unique<SnapshotRegistry>()) {
}

PlcMemory::~PlcMemory() = default;

void PlcMemory::attachState(PlcState& t_state) {
    p_plc_state_ = &t_state;

    if (p_plc_state_)
        p_plc_state_->setCacheEnabled(is_cache_enabled_);
}

PlcState* PlcMemory::state() const {
    return p_plc_state_;
}

Result<void, PlcMemoryError> PlcMemory::loadRegistry(const PlcSchemaStore& t_store) {
    std::lock_guard<std::mutex> lock(dirty_cv_mutex_);

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    uint16_t next_id = 1000;

    auto convert_to_node = [&](const DbField& t_field, auto& t_self_ref, DbEntry* t_entry, s7codec::Endian t_db_endian,
                               uint16_t t_db_num) -> PlcNode {
        PlcNode n;

        n.name_ = t_field.name;
        n.id_ = next_id++;
        n.cached_slot_ = t_entry;
        n.db_number_ = t_db_num;
        n.endian_ = t_field.endianness;
        n.is_dynamic_ = t_field.is_dynamic;

        if (t_field.type == DataType::Struct) {
            n.size_ = t_field.struct_size;
        } else if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
                   t_field.type == DataType::XWString) {

            // After the offset-tracker fix, struct_size is always the
            // per-element byte span (== typeSpanBytes(type, char_capacity)).
            // Use it directly. Fall back to fieldElementSpanBytes only for
            // truly legacy fields that arrive without struct_size set.
            n.size_ = static_cast<size_t>(
                t_field.struct_size > 0
                    ? t_field.struct_size
                    : s7codec::typeSpanBytes(t_field.type, t_field.string_capacity > 0 ? t_field.string_capacity : t_field.count)
                          .value_or(0));
        } else {
            n.size_ = static_cast<size_t>(s7codec::primitiveSize(t_field.type).value_or(0));
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
        n.string_capacity_ = static_cast<uint32_t>(t_field.string_capacity);
        n.enum_map_ = t_field.enum_map;
        n.min_val_ = t_field.min_val;
        n.max_val_ = t_field.max_val;

        for (const auto& child : t_field.children) {
            n.children_.push_back(t_self_ref(child, t_self_ref, t_entry, t_db_endian, t_db_num));
        }

        return n;
    };

    uint16_t max_db_num = 0;
    for (uint16_t num : t_store.availableDbs()) {
        if (num > max_db_num)
            max_db_num = num;
    }
    if (snapshot_registry_) {
        snapshot_registry_->ensureCapacity(max_db_num);
    }

    for (uint16_t num : t_store.availableDbs()) {
        auto res = t_store.getDb(num);

        if (res.hasError() || !res.value() || res.value()->size_bytes <= 0)
            continue;

        const auto* schema = res.value();

        std::string db_path = schema->db_name.empty() ? fmt::format("DB{}", num) : schema->db_name;

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
            n.children_.push_back(convert_to_node(t_field, convert_to_node, p_entry, schema->endianness, num));
        }

        p_plc_state_->add(std::move(n), "");

        // Seed `:=` initializers into the freshly zeroed segment.
        // This must happen after add(), once the segment arena for this DB
        // is registered.
        applyDbFieldInits(*this, num, schema->fields);
    }

    p_plc_state_->rebuildFieldIndex();

    return {};
}

Result<void, PlcMemoryError> PlcMemory::registerDb(uint16_t t_db_number, size_t t_size) {

    std::lock_guard<std::mutex> lock(dirty_cv_mutex_);

    if (!p_plc_state_)
        return PlcMemoryError::PLC_STATE_NOT_INITIALIZED;

    std::string path = fmt::format("DB{}", t_db_number);

    p_plc_state_->registerSegment(path, t_db_number, t_size);

    DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    if (!p_entry)
        return PlcMemoryError::DB_SEGMENT_NOT_FOUND;

    PlcNode node;

    node.name_ = path;
    node.id_ = t_db_number;
    node.size_ = t_size;
    node.cached_slot_ = p_entry;
    node.db_number_ = t_db_number;

    p_plc_state_->add(std::move(node), "");

    p_plc_state_->rebuildFieldIndex();

    return {};
}

void PlcMemory::buildRangeIndex() {
    std::unique_lock lock(index_mutex_);
    range_index_.clear();
    if (!p_plc_state_)
        return;

    for (const auto& [path, node_ptr] : p_plc_state_->nodes()) {
        if (!node_ptr || !node_ptr->children_.empty())
            continue; // only leaves
        if (!node_ptr->cached_slot_)
            continue;
        RangeEntry entry;
        entry.db = node_ptr->db_number_;
        entry.offset = node_ptr->offset_;
        entry.size = node_ptr->size_;
        entry.node = node_ptr.get();
        entry.node_span = node_ptr->size_; // or compute properly for arrays
        range_index_.push_back(entry);
    }

    std::sort(range_index_.begin(), range_index_.end(), [](const RangeEntry& a, const RangeEntry& b) {
        if (a.db != b.db)
            return a.db < b.db;
        return a.offset < b.offset;
    });
}

} // namespace sgrn::gateway::twin
