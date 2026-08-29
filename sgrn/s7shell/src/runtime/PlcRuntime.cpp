#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>
#include <sgrn/s7shell/utils/PlcSimClock.hpp>
#include <sgrn/scripting/ScriptHost.hpp>
#include <sgrn/utils/filesystem.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <algorithm>
#include <filesystem>
#include <utility>
namespace sgrn::s7shell::runtime
{

using sgrn::utils::filesystem::expandUserPath;

PlcRuntime::PlcRuntime() {
    // Every DB written through this runtime should carry a consistent
    // timestamp regardless of which protocol endpoint wrote it.
    memory_.setTimestampProvider([]() { return static_cast<uint64_t>(::sgrn::s7shell::shell::g_plc_clock.nowMs()); });
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
    fmt::print(fg(fmt::color::green), "[PlcRuntime] Loaded schema from {} ({} DBs)\n", expanded, schema_.dbs().size());
    if (shell::p_g_as_engine) {
        sgrn::scripting::ScriptHost host(shell::p_g_as_engine);
        shell::registerSchemaTypes(host, schema_); // re-register everything
        shell::registerDbPropertyAccessors(host, schema_);
        fmt::print(fg(fmt::color::green), "[PlcRuntime] Schema types registered for AngelScript\n");
    }
}

void PlcRuntime::loadJsonSchema(const std::string& t_path) {
    std::string expanded = expandUserPath(t_path);
    auto res = schema_.loadFromJsonFile(expanded);
    if (res.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] Failed to load JSON schema from {}: {}\n", expanded,
            sgrn::scl::toString(res.error()));
        return;
    }
    (void)memory_.loadRegistry(schema_);
    fmt::print(fg(fmt::color::green), "[PlcRuntime] Loaded JSON schema from {} ({} DBs)\n", expanded, schema_.dbs().size());
    if (shell::p_g_as_engine) {
        sgrn::scripting::ScriptHost host(shell::p_g_as_engine);
        shell::registerSchemaTypes(host, schema_);
        shell::registerDbPropertyAccessors(host, schema_);
        fmt::print(fg(fmt::color::green), "[PlcRuntime] Schema types registered for AngelScript\n");
    }
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

} // namespace sgrn::s7shell::runtime
