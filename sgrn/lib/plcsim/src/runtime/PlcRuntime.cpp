#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/plcsim/utils/PlcSimClock.hpp>
#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/filesystem.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <snap7.h>
#include <stdexcept>
#include <string>
#include <utility>
namespace sgrn::plcsim::runtime
{

using sgrn::utils::filesystem::expandUserPath;
PlcRuntime::PlcRuntime() {
    // Every DB written through this runtime should carry a consistent
    // timestamp regardless of which protocol endpoint wrote it.
    memory_.setTimestampProvider([]() { return static_cast<uint64_t>(::sgrn::plcsim::utils::g_plc_clock.nowMs()); });
    memory_.attachState(state_);
}

std::shared_ptr<PlcRuntime> PlcRuntime::empty() {
    return std::make_shared<PlcRuntime>();
}

std::shared_ptr<PlcRuntime> PlcRuntime::fromSclSchema(const std::string& t_path) {
    auto rt = empty();
    rt->loadSclSchema(t_path);
    return rt;
}

std::shared_ptr<PlcRuntime> PlcRuntime::fromJsonSchema(const std::string& t_path) {
    auto rt = empty();
    rt->loadJsonSchema(t_path);
    return rt;
}

void PlcRuntime::loadSclSchema(const std::string& t_path) {
    std::string expanded = expandUserPath(t_path);
    auto res = schema_.loadSchema(expanded); // Note: schema.loadSchema, not loadFile
    if (res.hasError()) {
        fmt::print(
            stderr, fg(fmt::color::red), "[PlcRuntime] Failed to load schema from {}: {}\n", expanded, sgrn::scl::toString(res.error()));
        return;
    }
    auto r = memory_.loadRegistry(schema_);
    if (r.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] loadRegistry failed: {}\n", toString(r.error()));
        return;
    }
    // Tags after memory (arenas are independent of DB memory).
    importSchemaTags();
    fmt::print(fg(fmt::color::green), "[PlcRuntime] Loaded schema from {} ({} DBs)\n", expanded, schema_.dbs().size());
    if (g_on_schema_loaded)
        g_on_schema_loaded(*this);
}

void PlcRuntime::loadJsonSchema(const std::string& t_path) {
    std::string expanded = expandUserPath(t_path);
    // loadFromJsonFile is a static factory: the result must be moved into
    // schema_ (previously discarded, so JSON schemas silently loaded empty).
    auto res = PlcSchemaStore::loadFromJsonFile(expanded);
    if (res.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] Failed to load JSON schema from {}: {}\n", expanded,
            sgrn::scl::toString(res.error()));
        return;
    }
    schema_ = std::move(res.value());
    (void)memory_.loadRegistry(schema_);
    importSchemaTags();
    fmt::print(fg(fmt::color::green), "[PlcRuntime] Loaded JSON schema from {} ({} DBs)\n", expanded, schema_.dbs().size());
    if (g_on_schema_loaded)
        g_on_schema_loaded(*this);
}

void PlcRuntime::registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name) {
    ::sgrn::scl::DbSchema db;
    db.db_number = t_num;
    db.db_name = t_name.empty() ? fmt::format("DB{}", t_num) : t_name;
    db.size_bytes = static_cast<int>(t_size);
    (void)schema_.addDb(std::move(db), true);
    (void)memory_.loadRegistry(schema_);
}

void PlcRuntime::registerUdt(const std::string& t_name, uint32_t t_size) {
    ::sgrn::scl::UdtDefinition udt;
    udt.name = t_name;
    udt.size_bytes = static_cast<int>(t_size);
    (void)schema_.addUdt(std::move(udt), true);
}

void PlcRuntime::addUdtField(
    const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count) {
    auto res = schema_.getUdtByName(t_udt_name);
    if (res.hasError())
        return;

    ::sgrn::scl::UdtDefinition udt = *res.value();
    ::sgrn::scl::DbField field;
    field.name = t_name;
    field.offset = static_cast<int>(t_offset);
    field.count = static_cast<int>(t_count);

    if (auto t = ::sgrn::scl::parseS7Type(t_type_str)) {
        field.type = *t;
    } else if (schema_.hasUdt(t_type_str)) {
        field.udt_name = t_type_str;
        // Recursively pull children if it's a known UDT
        if (auto sub = schema_.getUdtByName(t_type_str); !sub.hasError()) {
            field.children = sub.value()->fields;
            field.struct_size = sub.value()->size_bytes;
        }
    }

    udt.fields.push_back(std::move(field));
    (void)schema_.addUdt(std::move(udt), true);
}

void PlcRuntime::loadRegistry(const std::string& t_path_or_content) {
    if (t_path_or_content.empty()) {
        fmt::print(stderr, fg(fmt::color::yellow), "[PlcRuntime] loadRegistry: empty path, ignored.\n");
        return;
    }
    std::string expanded = expandUserPath(t_path_or_content);
    if (!std::filesystem::exists(expanded)) {
        fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] loadRegistry: file not found: {}\n", expanded);
        return;
    }
    tag_table_ = std::make_unique<PlcTagTable>(expanded);
    fmt::print(fg(fmt::color::green), "[PlcRuntime] Loaded registry from {}\n", expanded);
}

DbIOProvider* PlcRuntime::getOrCreateDbProvider(uint16_t t_db_num) {
    auto it = db_providers_.find(t_db_num);
    if (it != db_providers_.end()) {
        return it->second.get();
    }
    auto schema_db = schema_.getDb(t_db_num);
    if (schema_db.hasError()) {
        return nullptr;
    }
    auto provider = std::make_unique<DbIOProvider>(memory_, schema_, t_db_num, pending_writes_);
    auto* p_ptr = provider.get();
    db_providers_.emplace(t_db_num, std::move(provider));
    return p_ptr;
}

// ---------------------------------------------------------------------------
// Dirty-region tracking
//
// Coalesces overlapping/adjacent regions per-DB so that a push cycle (an S7
// write-back, a proxy forward, a future protocol server's change
// notification) can ask "what changed since I last looked" without every
// writer needing to know about every reader.
// ---------------------------------------------------------------------------

void PlcRuntime::markDirty(uint16_t t_db_num, uint32_t t_offset, uint32_t t_length) {
    if (t_length == 0)
        return;
    bool merged = false;
    {
        std::lock_guard<std::mutex> lk(dirty_mutex_);
        auto& regions = dirty_regions_[t_db_num];

        uint32_t new_end = t_offset + t_length;
        for (auto& r : regions) {
            uint32_t r_end = r.offset + r.length;
            // Merge if overlapping or contiguous.
            if (t_offset <= r_end && new_end >= r.offset) {
                uint32_t merged_start = std::min(r.offset, t_offset);
                uint32_t merged_end = std::max(r_end, new_end);
                r.offset = merged_start;
                r.length = merged_end - merged_start;
                merged = true;
                break;
            }
        }
        if (!merged)
            regions.push_back(DirtyRegion{t_offset, t_length});
    }

    // Observer callbacks are deliberately invoked after releasing
    // dirty_mutex_. GatewayBinding observers wake an HTTP publish worker, and
    // failed publishes may later re-enter markDirty() to restore unsent
    // regions. Holding the dirty ledger lock across callbacks would create a
    // lock-ordering trap between runtime writers and protocol bindings.
    std::vector<DirtyObserver> observers;
    {
        std::lock_guard<std::mutex> lk(observer_mutex_);
        observers.reserve(dirty_observers_.size());
        for (const auto& [_, t_observer] : dirty_observers_)
            observers.push_back(t_observer);
    }
    for (const auto& t_observer : observers) {
        if (t_observer)
            t_observer(t_db_num, t_offset, t_length);
    }
}

std::vector<DirtyRegion> PlcRuntime::takeDirty(uint16_t t_db_num) {
    std::lock_guard<std::mutex> lk(dirty_mutex_);
    auto it = dirty_regions_.find(t_db_num);
    if (it == dirty_regions_.end())
        return {};
    std::vector<DirtyRegion> out = std::move(it->second);
    dirty_regions_.erase(it);
    return out;
}

bool PlcRuntime::hasDirty(uint16_t t_db_num) const {
    std::lock_guard<std::mutex> lk(dirty_mutex_);
    auto it = dirty_regions_.find(t_db_num);
    return it != dirty_regions_.end() && !it->second.empty();
}

size_t PlcRuntime::addDirtyObserver(DirtyObserver t_observer) {
    if (!t_observer)
        return 0;
    const size_t t_id = next_observer_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(observer_mutex_);
    dirty_observers_.emplace(t_id, std::move(t_observer));
    return t_id;
}

void PlcRuntime::removeDirtyObserver(size_t t_id) {
    std::lock_guard<std::mutex> lk(observer_mutex_);
    dirty_observers_.erase(t_id);
}

// ---------------------------------------------------------------------------
// Discrete areas + TIA-style tag table
// ---------------------------------------------------------------------------
namespace
{
bool isDiscreteArea(int t_area) {
    return t_area == S7AreaPE || t_area == S7AreaPA || t_area == S7AreaMK;
}

const char* discreteAreaName(int t_area) {
    switch (t_area) {
        case S7AreaPE:
            return "PE";
        case S7AreaPA:
            return "PA";
        case S7AreaMK:
            return "MK";
        default:
            return "?";
    }
}
} // namespace

sgrn::Result<void, std::string> PlcRuntime::ensureAreaSize(int t_area, size_t t_min_size) {
    // Caller holds tag_mutex_.
    if (t_area == S7AreaDB)
        return std::string("internal error: DB areas are twin-owned, not arena-backed");
    if (!isDiscreteArea(t_area))
        return fmt::format("area code {} is not backed (PE/PA/MK only)", t_area);
    if (t_min_size > kMaxAreaSize)
        return fmt::format("address {} bytes exceeds area cap of {} bytes", t_min_size, kMaxAreaSize);
    auto& arena = areas_[t_area];
    const size_t want = std::max(t_min_size, arena.empty() ? kDefaultAreaSize : arena.size());
    if (want > arena.size()) {
        if (!arena.empty())
            fmt::print(
                fg(fmt::color::yellow), "[PlcRuntime] Growing {} area to {} bytes for tag address.\n", discreteAreaName(t_area), want);
        arena.assign(want, 0);
    }
    return {};
}

sgrn::Result<void, std::string> PlcRuntime::defineTag(
    const std::string& t_name, const std::string& t_type_str, const std::string& t_addr_str, const std::string& t_table) {
    if (t_name.empty())
        return std::string("defineTag: tag name must not be empty");

    auto addr = ::sgrn::scl::parsePlcAddress(t_addr_str);
    if (!addr.has_value())
        return fmt::format(
            "defineTag('{}'): cannot parse address '{}' (expected %I0.0, %Q0.0, IB3, %IW6, %MD8, DB1.DBX0.0)", t_name, t_addr_str);
    return defineTagResolved(t_name, t_type_str, *addr, t_table);
}

sgrn::Result<void, std::string> PlcRuntime::defineTagResolved(
    const std::string& t_name, const std::string& t_type_str, const ::sgrn::scl::PlcAddress& t_addr, const std::string& t_table) {
    if (t_name.empty())
        return std::string("defineTag: tag name must not be empty");

    // The DB area is defined exclusively through DATA_BLOCK syntax — tags
    // cover the discrete areas (PE/PA/MK) only.
    if (t_addr.area == S7AreaDB)
        return fmt::format("defineTag('{}'): DB addresses belong in DATA_BLOCK syntax, not tag tables", t_name);

    const ::sgrn::scl::PlcAddress& addr = t_addr;
    RuntimeTag tag;
    tag.name = t_name;
    tag.table = t_table;
    tag.type_str = t_type_str;
    tag.addr = addr;
    bool is_udt = false;
    if (auto t = ::sgrn::scl::parseS7Type(t_type_str)) {
        tag.type = *t;
        if (tag.type == ::sgrn::scl::DataType::Timer || tag.type == ::sgrn::scl::DataType::Counter)
            return fmt::format("defineTag('{}'): Timer/Counter areas (T/C) are not backed yet", t_name);
        if (tag.type == ::sgrn::scl::DataType::String || tag.type == ::sgrn::scl::DataType::WString ||
            tag.type == ::sgrn::scl::DataType::XString || tag.type == ::sgrn::scl::DataType::XWString)
            return fmt::format("defineTag('{}'): string types are not supported for tags yet", t_name);
        const int span = ::sgrn::scl::rawTypeSpanBytes(tag.type, 1);
        if (span <= 0)
            return fmt::format("defineTag('{}'): cannot size type '{}'", t_name, t_type_str);
        tag.span_bytes = span;
    } else if (auto u = schema_.getUdtByName(t_type_str); !u.hasError() && u.value()) {
        is_udt = true;
        tag.type = ::sgrn::scl::DataType::Struct;
        tag.udt_name = t_type_str;
        tag.span_bytes = u.value()->size_bytes;
        tag.field.children = u.value()->fields;
        tag.field.struct_size = static_cast<uint32_t>(u.value()->size_bytes);
        if (tag.span_bytes <= 0)
            return fmt::format("defineTag('{}'): UDT '{}' has zero size", t_name, t_type_str);
    } else {
        return fmt::format("defineTag('{}'): unknown type '{}' (not a scalar, not a schema UDT)", t_name, t_type_str);
    }

    const bool is_bool = !is_udt && tag.type == ::sgrn::scl::DataType::Bool;
    if (is_bool && tag.addr.bit_index < 0)
        return fmt::format("defineTag('{}'): Bool tags need a bit address (e.g. %I0.0)", t_name);
    if (!is_bool && tag.addr.bit_index >= 0)
        return fmt::format("defineTag('{}'): non-Bool type '{}' needs a byte/word/dword address", t_name, t_type_str);
    if (!is_udt && tag.span_bytes != tag.addr.byte_count)
        return fmt::format("defineTag('{}'): type '{}' ({} bytes) does not match address width ({} bytes)", t_name, t_type_str,
            tag.span_bytes, tag.addr.byte_count);

    tag.field.name = t_name;
    tag.field.type = tag.type;
    tag.field.count = 1;
    tag.field.endianness = s7codec::Endian::Big;

    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        if (tags_.count(t_name))
            return fmt::format("defineTag('{}'): duplicate tag name", t_name);

        const size_t end = static_cast<size_t>(tag.addr.byte_offset) + static_cast<size_t>(tag.span_bytes);
        if (auto r = ensureAreaSize(tag.addr.area, end); r.hasError())
            return fmt::format("defineTag('{}'): {}", t_name, r.error());

        for (const auto& [other_name, other] : tags_) {
            if (other.addr.area != tag.addr.area)
                continue;
            const size_t o_begin = static_cast<size_t>(other.addr.byte_offset);
            const size_t o_end = o_begin + static_cast<size_t>(other.span_bytes);
            const size_t begin = static_cast<size_t>(tag.addr.byte_offset);
            if (begin < o_end && end > o_begin)
                fmt::print(stderr, fg(fmt::color::yellow), "[PlcRuntime] defineTag('{}'): overlaps tag '{}', sharing bytes.\n", t_name,
                    other_name);
        }

        // Print before the move: tag is consumed by emplace below.
        fmt::print(fg(fmt::color::green), "[PlcRuntime] Tag '{}' defined ({} @ {}).\n", t_name, t_type_str, tag.addr.label);
        tags_.emplace(t_name, std::move(tag));
    }

    markTagDirty(t_name);
    return {};
}

bool PlcRuntime::hasTag(const std::string& t_name) const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    return tags_.count(t_name) != 0;
}

std::vector<std::string> PlcRuntime::tagNames() const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> names;
    names.reserve(tags_.size());
    for (const auto& [name, _] : tags_)
        names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> PlcRuntime::tagNamesInTable(const std::string& t_table) const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> names;
    for (const auto& [name, tag] : tags_) {
        if (tag.table == t_table)
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> PlcRuntime::tagTables() const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> tables;
    for (const auto& [_, tag] : tags_) {
        if (!tag.table.empty() && std::find(tables.begin(), tables.end(), tag.table) == tables.end())
            tables.push_back(tag.table);
    }
    std::sort(tables.begin(), tables.end());
    return tables;
}

void PlcRuntime::importSchemaTags() {
    // Schema (re)load is a fresh start for tags too, mirroring how DB memory
    // is re-initialized: drop everything, then define each schema row.
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        tags_.clear();
        areas_.clear();
        dirty_tags_.clear();
        publish_tags_.clear();
    }
    for (const auto& [name, tag] : schema_.tags()) {
        // defineTagResolved re-validates against the merged store (UDT sizes,
        // DB ranges) and reports precisely; a single bad row warns and skips
        // without killing the load.
        const std::string type = !tag.udt_name.empty() ? tag.udt_name : tag.type_str;
        if (auto r = defineTagResolved(tag.name, type, tag.addr, tag.table_name); r.hasError())
            fmt::print(stderr, fg(fmt::color::yellow), "[PlcRuntime] schema tag '{}' skipped: {}\n", tag.name, r.error());
    }
}

sgrn::Result<PlcRuntime::RuntimeTag, std::string> PlcRuntime::describeTag(const std::string& t_name) const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    auto it = tags_.find(t_name);
    if (it == tags_.end())
        return sgrn::Result<RuntimeTag, std::string>::Error("describeTag: unknown tag '" + t_name + "'");
    return it->second;
}

sgrn::Result<void, std::string> PlcRuntime::readAreaMemory(int t_area, size_t t_offset, size_t t_size, uint8_t* tp_buffer) const {
    if (t_area == S7AreaDB)
        return std::string("readAreaMemory: DB areas are twin-owned, use readDbMemory");
    if (t_size == 0)
        return {};
    std::lock_guard<std::mutex> lk(tag_mutex_);
    auto it = areas_.find(t_area);
    if (it == areas_.end() || t_offset + t_size > it->second.size())
        return fmt::format("area read out of range (area={}, off={}, size={})", t_area, t_offset, t_size);
    std::memcpy(tp_buffer, it->second.data() + t_offset, t_size);
    return {};
}

sgrn::Result<void, std::string> PlcRuntime::writeAreaMemory(
    int t_area, size_t t_offset, size_t t_size, const uint8_t* tp_data, bool t_mark_dirty) {
    if (t_area == S7AreaDB)
        return std::string("writeAreaMemory: DB areas are twin-owned, use writeDbMemory");
    if (t_size == 0)
        return {};
    std::vector<std::string> overlapped;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = areas_.find(t_area);
        if (it == areas_.end() || t_offset + t_size > it->second.size())
            return fmt::format("area write out of range (area={}, off={}, size={})", t_area, t_offset, t_size);
        std::memcpy(it->second.data() + t_offset, tp_data, t_size);
        // Uplink applies (gateway write_area) pass t_mark_dirty=false: like
        // twin DB writes they must not dirty-mark, or every applied uplink
        // write broadcasts back to its sender and the sync republish loop
        // spins forever on its own echo.
        if (t_mark_dirty) {
            for (const auto& [name, tag] : tags_) {
                if (tag.addr.area != t_area)
                    continue;
                const size_t begin = static_cast<size_t>(tag.addr.byte_offset);
                if (t_offset < begin + static_cast<size_t>(tag.span_bytes) && t_offset + t_size > begin)
                    overlapped.push_back(name);
            }
            for (const auto& name : overlapped) {
                dirty_tags_.insert(name);
                publish_tags_.insert(name);
            }
        }
    }
    if (t_mark_dirty)
        notifyTagObservers(overlapped);
    return {};
}

sgrn::Result<void, std::string> PlcRuntime::writeAreaBit(int t_area, size_t t_byte_offset, int t_bit_index, bool t_value) {
    if (t_area == S7AreaDB)
        return std::string("writeAreaBit: DB areas are twin-owned, use writeBit");
    if (t_bit_index < 0 || t_bit_index > 7)
        return fmt::format("bit index {} out of range 0..7", t_bit_index);
    std::vector<std::string> overlapped;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = areas_.find(t_area);
        if (it == areas_.end() || t_byte_offset >= it->second.size())
            return fmt::format("area bit write out of range (area={}, byte={})", t_area, t_byte_offset);
        uint8_t& byte = it->second[t_byte_offset];
        if (t_value)
            byte = static_cast<uint8_t>(byte | (1u << t_bit_index));
        else
            byte = static_cast<uint8_t>(byte & ~(1u << t_bit_index));
        for (const auto& [name, tag] : tags_) {
            if (tag.addr.area == t_area && static_cast<size_t>(tag.addr.byte_offset) == t_byte_offset)
                overlapped.push_back(name);
        }
        for (const auto& name : overlapped) {
            dirty_tags_.insert(name);
            publish_tags_.insert(name);
        }
    }
    notifyTagObservers(overlapped);
    return {};
}

size_t PlcRuntime::areaSize(int t_area) const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    auto it = areas_.find(t_area);
    return it == areas_.end() ? 0 : it->second.size();
}

namespace
{
bool parseTagBool(const std::string& t_json_val, bool& t_out) {
    std::string v = t_json_val;
    // trim + strip one layer of quotes
    const auto not_space = [](char c) { return c != ' ' && c != '\t' && c != '\n' && c != '\r'; };
    v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
    v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    if (v == "true" || v == "TRUE" || v == "1") {
        t_out = true;
        return true;
    }
    if (v == "false" || v == "FALSE" || v == "0") {
        t_out = false;
        return true;
    }
    return false;
}
} // namespace

sgrn::Result<std::string, ::sgrn::scl::SclError> PlcRuntime::readTagJson(const std::string& t_name) const {
    using ::sgrn::scl::SclError;
    RuntimeTag tag;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = tags_.find(t_name);
        if (it == tags_.end())
            return SclError::NotFound;
        tag = it->second;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(tag.span_bytes), 0);
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = areas_.find(tag.addr.area);
        if (it == areas_.end() || static_cast<size_t>(tag.addr.byte_offset) + buf.size() > it->second.size())
            return SclError::Generic;
        std::memcpy(buf.data(), it->second.data() + tag.addr.byte_offset, buf.size());
    }

    if (tag.type == ::sgrn::scl::DataType::Bool) {
        const bool bit = (buf[0] & (1u << tag.addr.bit_index)) != 0;
        return std::string(bit ? "true" : "false");
    }
    auto decoded = ::sgrn::gateway::twin::decodeFieldAt(tag.field, buf.data(), buf.size());
    if (decoded.hasError())
        return decoded.error();
    return decoded.value();
}

sgrn::Result<void, std::string> PlcRuntime::writeTagJson(const std::string& t_name, const std::string& t_value_json) {
    RuntimeTag tag;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = tags_.find(t_name);
        if (it == tags_.end())
            return fmt::format("writeTag: unknown tag '{}'", t_name);
        tag = it->second;
    }

    if (tag.type == ::sgrn::scl::DataType::Bool) {
        bool val = false;
        if (!parseTagBool(t_value_json, val))
            return fmt::format("writeTag('{}'): cannot parse '{}' as Bool", t_name, t_value_json);
        if (auto r = writeAreaBit(tag.addr.area, static_cast<size_t>(tag.addr.byte_offset), tag.addr.bit_index, val); r.hasError())
            return fmt::format("writeTag('{}'): {}", t_name, r.error());
        return {};
    }

    std::vector<uint8_t> buf(static_cast<size_t>(tag.span_bytes), 0);
    // Preserve sibling bits/bytes for struct tags sharing the area.
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        auto it = areas_.find(tag.addr.area);
        if (it == areas_.end() || static_cast<size_t>(tag.addr.byte_offset) + buf.size() > it->second.size())
            return fmt::format("writeTag('{}'): tag range out of area", t_name);
        std::memcpy(buf.data(), it->second.data() + tag.addr.byte_offset, buf.size());
    }
    if (auto r = ::sgrn::gateway::twin::encodeFieldAt(tag.field, t_value_json, buf.data(), buf.size(), 0, tag.field.endianness);
        r.hasError())
        return fmt::format("writeTag('{}'): encode failed ({})", t_name, ::sgrn::scl::toString(r.error()));

    if (auto r = writeAreaMemory(tag.addr.area, static_cast<size_t>(tag.addr.byte_offset), buf.size(), buf.data()); r.hasError())
        return fmt::format("writeTag('{}'): {}", t_name, r.error());
    return {};
}

void PlcRuntime::markTagDirty(const std::string& t_name) {
    std::vector<TagDirtyObserver> observers;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        if (!tags_.count(t_name))
            return;
        dirty_tags_.insert(t_name);
        publish_tags_.insert(t_name);
    }
    {
        std::lock_guard<std::mutex> lk(tag_observer_mutex_);
        observers.reserve(tag_observers_.size());
        for (const auto& [_, obs] : tag_observers_)
            observers.push_back(obs);
    }
    for (const auto& obs : observers) {
        if (obs)
            obs(t_name);
    }
}

void PlcRuntime::notifyTagObservers(const std::vector<std::string>& t_names) {
    if (t_names.empty())
        return;
    std::vector<TagDirtyObserver> observers;
    {
        std::lock_guard<std::mutex> lk(tag_observer_mutex_);
        observers.reserve(tag_observers_.size());
        for (const auto& [_, obs] : tag_observers_)
            observers.push_back(obs);
    }
    for (const auto& name : t_names) {
        for (const auto& obs : observers) {
            if (obs)
                obs(name);
        }
    }
}

std::vector<std::string> PlcRuntime::takeDirtyTags() {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> out(dirty_tags_.begin(), dirty_tags_.end());
    dirty_tags_.clear();
    std::sort(out.begin(), out.end());
    return out;
}

bool PlcRuntime::hasDirtyTags() const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    return !dirty_tags_.empty();
}

std::vector<std::string> PlcRuntime::takePublishTags() {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> out(publish_tags_.begin(), publish_tags_.end());
    publish_tags_.clear();
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> PlcRuntime::peekPublishTags() const {
    std::lock_guard<std::mutex> lk(tag_mutex_);
    std::vector<std::string> out(publish_tags_.begin(), publish_tags_.end());
    std::sort(out.begin(), out.end());
    return out;
}

void PlcRuntime::restorePublishTags(const std::vector<std::string>& t_names) {
    if (t_names.empty())
        return;
    std::lock_guard<std::mutex> lk(tag_mutex_);
    for (const auto& name : t_names) {
        if (tags_.count(name))
            publish_tags_.insert(name);
    }
}

void PlcRuntime::renotifyDirtyTags() {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(tag_mutex_);
        names.assign(dirty_tags_.begin(), dirty_tags_.end());
    }
    notifyTagObservers(names);
}

size_t PlcRuntime::addTagDirtyObserver(TagDirtyObserver t_observer) {
    if (!t_observer)
        return 0;
    const size_t t_id = next_tag_observer_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(tag_observer_mutex_);
    tag_observers_.emplace(t_id, std::move(t_observer));
    return t_id;
}

void PlcRuntime::removeTagDirtyObserver(size_t t_id) {
    std::lock_guard<std::mutex> lk(tag_observer_mutex_);
    tag_observers_.erase(t_id);
}

} // namespace sgrn::plcsim::runtime
