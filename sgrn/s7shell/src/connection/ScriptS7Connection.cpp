#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptAsync.hpp>
#include <sgrn/s7shell/facades/ScriptBlocks.hpp>
#include <sgrn/s7shell/facades/ScriptConnectionProxy.hpp>
#include <sgrn/s7shell/facades/ScriptDiagnostics.hpp>
#include <sgrn/s7shell/facades/ScriptMemory.hpp>
#include <sgrn/s7shell/facades/ScriptPlcControl.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptSchemaStore.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/PlcSimClock.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <algorithm>
#include <angelscript.h>
#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <stdexcept>
#include <thread>

namespace sgrn::s7shell::shell
{
using sgrn::s7shell::runtime::PlcRuntimeSPtr;

using sgrn::gateway::wrappers::s7::S7Error;

using sgrn::scl::SclError;

static SclError proto_bridge(const S7Error& t_e) {
    SclError mapped_code = SclError::Generic;
    switch (t_e) {
        case S7Error::Success:
        case S7Error::ConnectionFailed:
        case S7Error::Timeout:
        case S7Error::PduError:
        case S7Error::ReadError:
        case S7Error::WriteError:
        case S7Error::DeviceBusy:
        case S7Error::NotConnected:
        case S7Error::Unknown:
            mapped_code = SclError::Generic;
            break;
        case S7Error::InvalidParam:
            mapped_code = SclError::InvalidType; // closest schema equivalent
            break;
    }
    return mapped_code;
}

ScriptS7Connection::ScriptS7Connection(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port)
    : ScriptS7Connection(t_ip, t_rack, t_slot, t_port, ::sgrn::s7shell::runtime::PlcRuntime::empty()) {
}

ScriptS7Connection::ScriptS7Connection(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, PlcRuntimeSPtr tsp_rt)
    : runtime_(std::move(tsp_rt))
    , memory_(runtime_->getMemory())
    , schema_(runtime_->getSchema())
    , tag_table_(runtime_->getTagTableSlot())
    , pending_writes_(runtime_->getPendingWrites())
    , db_snapshots_(runtime_->getDbSnapshots())
    , conn_ip_(t_ip)
    , conn_rack_(t_rack)
    , conn_slot_(t_slot)
    , conn_port_(t_port) {
    // PlcRuntime owns and attaches its own PlcState at construction (see
    // PlcRuntime::PlcRuntime()); `memory_` is a reference into that shared
    // runtime, so it already has a live state the moment this connection
    // exists — no per-connection attach, no last-writer-wins race.
    client_.attachSchema(schema_);
    auto res = client_.connect(t_ip, t_rack, t_slot, conn_type_, conn_port_);
    if (res.hasError()) {
        setLastError(res.error());
    }
    p_g_active_connection = this;
}

ScriptS7Connection::~ScriptS7Connection() {
    if (p_g_active_connection == this)
        p_g_active_connection = nullptr;
    (void)client_.disconnect();
}

void ScriptS7Connection::loadRegistry(const std::string& t_path) {
    runtime_->loadRegistry(t_path);
}

sgrn::Result<void, SclError> ScriptS7Connection::reconnect() {
    (void)client_.disconnect();
    if (conn_use_tsap_) {
        auto res = client_.connectWithTsap(conn_ip_, conn_local_tsap_, conn_remote_tsap_);
        if (res.hasError())
            return proto_bridge(res.error());
        return {};
    }
    auto res = client_.connect(conn_ip_, conn_rack_, conn_slot_, conn_type_, conn_port_);
    if (res.hasError())
        return proto_bridge(res.error());
    return {};
}

void ScriptS7Connection::setConnectionSettings(uint16_t t_type, uint16_t t_port, bool t_reconnect_if_connected) {
    conn_type_ = t_type;
    conn_port_ = t_port;
    if (t_reconnect_if_connected && client_.isConnected()) {
        (void)reconnect();
    }
}

sgrn::Result<void, SclError> ScriptS7Connection::connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    conn_ip_ = t_ip;
    conn_use_tsap_ = true;
    conn_local_tsap_ = t_local_tsap;
    conn_remote_tsap_ = t_remote_tsap;
    (void)client_.disconnect();
    auto res = client_.connectWithTsap(t_ip, t_local_tsap, t_remote_tsap);
    if (res.hasError())
        return proto_bridge(res.error());
    return {};
}

void ScriptS7Connection::setTsapMode(uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    conn_use_tsap_ = true;
    conn_local_tsap_ = t_local_tsap;
    conn_remote_tsap_ = t_remote_tsap;
    if (client_.isConnected()) {
        (void)reconnect();
    }
}

void ScriptS7Connection::setRackSlotMode() {
    conn_use_tsap_ = false;
    if (client_.isConnected()) {
        (void)reconnect();
    }
}

// NOTE: schema_/memory_ loading, DB/UDT registration, and DB I/O provider
// creation used to be implemented directly here, against this connection's
// privately-owned `schema_`/`memory_`/`dbProviders`. That logic now lives in
// PlcRuntime (see src/runtime/PlcRuntime.cpp) since it has nothing to do
// with the S7 wire protocol — these are now thin delegations so that
// ScriptS7Client's existing calls (conn_->loadSclSchema(...), etc.) keep
// working unchanged whether this connection owns a private runtime or
// shares one with other connections/protocol endpoints.

void ScriptS7Connection::loadSclSchema(const std::string& t_path) {
    runtime_->loadSclSchema(t_path);
}

void ScriptS7Connection::loadJsonSchema(const std::string& t_path) {
    runtime_->loadJsonSchema(t_path);
}

void ScriptS7Connection::registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name) {
    runtime_->registerDb(t_num, t_size, t_name);
}

void ScriptS7Connection::registerUdt(const std::string& t_name, uint32_t t_size) {
    runtime_->registerUdt(t_name, t_size);
}

void ScriptS7Connection::addUdtField(
    const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count) {
    runtime_->addUdtField(t_udt_name, t_name, t_type_str, t_offset, t_count);
}

::sgrn::gateway::twin::DbIOProvider* ScriptS7Connection::getOrCreateDbProvider(uint16_t t_db_num) {
    return runtime_->getOrCreateDbProvider(t_db_num);
}

// ============================================================================
// ScriptS7Client Implementation
// ============================================================================

ScriptS7Client::ScriptS7Client(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port)
    : conn_(std::make_unique<ScriptS7Connection>(t_ip, t_rack, t_slot, t_port)) {
}

ScriptS7Client::ScriptS7Client(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, PlcRuntimeSPtr tsp_rt)
    : conn_(std::make_unique<ScriptS7Connection>(t_ip, t_rack, t_slot, t_port, std::move(tsp_rt))) {
}

runtime::PlcRuntimeSPtr ScriptS7Client::getRuntime() const {
    return conn_->runtime_;
}

void ScriptS7Client::addRef() {
    ++ref_count_;
}

void ScriptS7Client::release() {
    if (--ref_count_ == 0)
        delete this;
}

ScriptDataBlock* ScriptS7Client::db(uint16_t t_db_num) {
    return new ScriptDataBlock(conn_.get(), t_db_num);
}

ScriptDataBlock* ScriptS7Client::dbByName(const std::string& t_name) {
    auto res = conn_->schema_.getDbByName(t_name);
    if (!res.has_value() || res.hasError()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(toString(res.error()).data());
        }
        return nullptr;
    }
    return new ScriptDataBlock(conn_.get(), res.value()->db_number);
}

ScriptTagTable* ScriptS7Client::tags() {
    return new ScriptTagTable(conn_.get());
}

std::string ScriptS7Client::tagGet(const std::string& t_name) {
    if (!conn_->tag_table_)
        return "null";
    return shell::valueOr(conn_->tag_table_->get(conn_->client_, t_name), std::string{"null"});
}

double ScriptS7Client::tagGetReal(const std::string& t_name) {
    return shell::jsonScalarDouble(tagGet(t_name));
}

int32_t ScriptS7Client::tagGetInt(const std::string& t_name) {
    return shell::jsonScalarInt(tagGet(t_name));
}

bool ScriptS7Client::tagGetBool(const std::string& t_name) {
    return tagGet(t_name) == "true";
}

void ScriptS7Client::tagPut(const std::string& t_name, const std::string& t_raw_val) {
    if (!conn_->tag_table_)
        return;
    const std::string json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    (void)shell::ok(conn_->tag_table_->put(conn_->client_, t_name, json_val));
}

void ScriptS7Client::tagPutDouble(const std::string& t_name, double t_val) {
    if (!conn_->tag_table_)
        return;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    (void)shell::ok(conn_->tag_table_->put(conn_->client_, t_name, sb.GetString()));
}

void ScriptS7Client::tagPutInt(const std::string& t_name, int32_t t_val) {
    if (!conn_->tag_table_)
        return;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    (void)shell::ok(conn_->tag_table_->put(conn_->client_, t_name, sb.GetString()));
}

void ScriptS7Client::tagPutBool(const std::string& t_name, bool t_val) {
    if (!conn_->tag_table_)
        return;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Bool(t_val);
    (void)shell::ok(conn_->tag_table_->put(conn_->client_, t_name, sb.GetString()));
}

std::string ScriptS7Client::read(const std::string& t_target) {
    if (!conn_->client_.isConnected()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException("Client not connected");
        }
        return "null";
    }
    if (auto whole = conn_->schema_.resolveDbRef(t_target)) {
        auto res = conn_->memory_.getSubtreeJson(*whole, "");
        if (res.hasError()) {
            if (auto* p_ctx = asGetActiveContext()) {
                p_ctx->SetException(toString(res.error()).data());
            }
            return "null";
        }
        return res.value();
    }
    auto ft = conn_->schema_.parseFieldTarget(t_target);
    if (!ft) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("Symbolic path '{}' not found in schema_", t_target).c_str());
        }
        return "null";
    }
    auto* p_provider = conn_->getOrCreateDbProvider(ft->db_number);
    if (!p_provider) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("No provider for DB{}", ft->db_number).c_str());
        }
        return "null";
    }
    auto res = p_provider->get(conn_->client_, ft->field_path);
    if (res.hasError()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("Failed to read field '{}': {}", t_target, toString(res.error())).c_str());
        }
        return "null";
    }
    return res.value();
}

void ScriptS7Client::write(const std::string& t_target, const std::string& t_raw_val) {
    if (!conn_->client_.isConnected()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException("Client not connected");
        }
        return;
    }
    const std::string json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    if (auto whole = conn_->schema_.resolveDbRef(t_target)) {
        auto snap_res = conn_->schema_.getDb(*whole);
        if (snap_res.hasError()) {
            if (auto* p_ctx = asGetActiveContext()) {
                p_ctx->SetException(fmt::format("DB schema_ not found for '{}'", t_target).c_str());
            }
            return;
        }
        DbSnapshot block(*snap_res.value());
        auto read_res = block.read(conn_->client_);
        if (read_res.hasError()) {
            if (auto* p_ctx = asGetActiveContext()) {
                p_ctx->SetException(fmt::format("Failed to read DB '{}' before write: {}", *whole, toString(read_res.error())).c_str());
            }
            return;
        }
        auto update_res = block.updateFromJson(json_val);
        if (update_res.hasError()) {
            if (auto* p_ctx = asGetActiveContext()) {
                p_ctx->SetException(toString(update_res.error()).data());
            }
            return;
        }
        auto write_res = block.write(conn_->client_);
        if (write_res.hasError()) {
            if (auto* p_ctx = asGetActiveContext()) {
                p_ctx->SetException(fmt::format("Failed to write DB '{}': {}", *whole, toString(write_res.error())).c_str());
            }
        }
        return;
    }
    auto ft = conn_->schema_.parseFieldTarget(t_target);
    if (!ft) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("Symbolic path '{}' not found in schema_", t_target).c_str());
        }
        return;
    }
    auto* p_provider = conn_->getOrCreateDbProvider(ft->db_number);
    if (!p_provider) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("No provider for DB{}", ft->db_number).c_str());
        }
        return;
    }
    auto write_res = p_provider->put(conn_->client_, ft->field_path, json_val);
    if (write_res.hasError()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("Failed to write field '{}': {}", t_target, toString(write_res.error())).c_str());
        }
    }
}

void ScriptS7Client::write(const std::string& t_target, double t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    write(t_target, std::string(sb.GetString()));
}

void ScriptS7Client::write(const std::string& t_target, int32_t t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    write(t_target, std::string(sb.GetString()));
}

void ScriptS7Client::write(const std::string& t_target, bool t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Bool(t_val);
    write(t_target, std::string(sb.GetString()));
}

void ScriptS7Client::reconnect() {
    auto r = conn_->reconnect();
    if (r.hasError())
        conn_->setLastError(r.error());
    else
        conn_->clearLastError();
}

bool ScriptS7Client::reconnectOk() {
    auto r = conn_->reconnect();
    if (r.hasError()) {
        conn_->setLastError(r.error());
        return false;
    }
    conn_->clearLastError();
    return true;
}

bool ScriptS7Client::reconnectWithRetry(int t_max_attempts, int t_delay_ms) {
    if (t_max_attempts <= 0)
        t_max_attempts = 1;
    for (int attempt = 1; attempt <= t_max_attempts; ++attempt) {
        auto r = conn_->reconnect();
        if (!r.hasError()) {
            conn_->clearLastError();
            fmt::print("[S7] reconnectWithRetry: success on attempt {}/{}\n", attempt, t_max_attempts);
            return true;
        }
        conn_->setLastError(r.error());
        if (attempt < t_max_attempts && t_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(t_delay_ms));
    }
    return false;
}

bool ScriptS7Client::ping() {
    // Read 1 byte from MB0 (Merker Bit area) — minimal, non-intrusive liveness test.
    auto r = conn_->client_.readArea(S7AreaMK, 0, 0, 1, S7WLByte);
    if (r.hasError()) {
        conn_->setLastError(r.error());
        return false;
    }
    conn_->clearLastError();
    return true;
}

std::string ScriptS7Client::lastError() const {
    return conn_->last_error_msg_;
}

int ScriptS7Client::getLastErrorCode() const {
    return conn_->last_error_code_;
}

void ScriptS7Client::clearLastError() {
    conn_->clearLastError();
}

void ScriptS7Client::disconnect() {
    (void)conn_->client_.disconnect();
}

bool ScriptS7Client::isConnected() const {
    return conn_->client_.isConnected();
}

std::string ScriptS7Client::listSymbols(const std::string& t_ref) {
    auto d = conn_->schema_.resolveDbRef(t_ref);
    if (!d) {
        try {
            d = static_cast<uint16_t>(std::stoul(t_ref));
        } catch (...) {
        }
    }
    if (!d)
        return "Unknown DB or symbol reference";
    auto res = conn_->schema_.getDb(*d);
    if (res.hasError())
        return "DB not loaded";
    const auto& db = *res.value();

    std::string out = fmt::format("DB{} '{}' ({} bytes):\n", *d, db.db_name, db.size_bytes);
    std::function<void(const std::vector<::sgrn::scl::DbField>&, int)> print_fields;
    print_fields = [&](const std::vector<::sgrn::scl::DbField>& t_fields, int t_depth) {
        for (const auto& f : t_fields) {
            const std::string indent(t_depth * 2, ' ');
            out += fmt::format("  {}{:<20} offset={:<4} bit={} type={:<10}", indent, f.name, f.offset, f.bit_index, f.type);
            if (!f.udt_name.empty())
                out += fmt::format(" udt={}", f.udt_name);
            if (f.count > 1)
                out += fmt::format(" count={}", f.count);
            out += "\n";
            if (!f.children.empty())
                print_fields(f.children, t_depth + 1);
        }
    };
    print_fields(db.fields, 0);
    return out;
}

std::string ScriptS7Client::searchSymbols(const std::string& t_regex_str) {
    try {
        std::regex re(t_regex_str, std::regex_constants::icase);
        std::string out = fmt::format("Search results for '{}':\n", t_regex_str);
        for (const auto& [n, db] : conn_->schema_.dbs()) {
            if (std::regex_search(db.db_name, re))
                out += fmt::format("  DB{:<4} {} (DB Name)\n", n, db.db_name);
            for (const auto& f : db.fields)
                if (std::regex_search(f.name, re))
                    out += fmt::format("  DB{:<4}.{:<20} type={:<10}\n", n, f.name, f.type);
        }
        return out;
    } catch (const std::regex_error& t_e) {
        return fmt::format("Invalid regex: {}", t_e.what());
    }
}

ScriptS7Diagnostics* ScriptS7Client::diagnostics() {
    return new ScriptS7Diagnostics(conn_.get());
}

ScriptS7PlcControl* ScriptS7Client::control() {
    return new ScriptS7PlcControl(conn_.get());
}

ScriptS7ConnectionProxy* ScriptS7Client::connection() {
    return new ScriptS7ConnectionProxy(conn_.get());
}

ScriptS7Blocks* ScriptS7Client::blocks() {
    return new ScriptS7Blocks(conn_.get());
}

ScriptS7Async* ScriptS7Client::asyncIo() {
    return new ScriptS7Async(conn_.get());
}

ScriptS7Memory* ScriptS7Client::memory_() {
    return new ScriptS7Memory(conn_.get());
}

static void registerSchemaFactories_(asIScriptEngine* tp_engine) {
    // No-op: casting via opCast is used instead to avoid return type overloading conflicts
}

ScriptSchemaStore* ScriptS7Client::schema_() {
    if (!conn_)
        return nullptr;
    auto* p_store = new ScriptSchemaStore(&conn_->schema_);
    p_store->addRef();
    return p_store;
}

void ScriptS7Client::loadSclSchema(const std::string& t_path) {
    conn_->loadSclSchema(t_path);
    if (p_g_as_engine) {
        sgrn::scripting::ScriptHost host(p_g_as_engine);
        registerSchemaTypes(host, conn_->schema_);
        registerSchemaFactories_(p_g_as_engine);
    }
}

void ScriptS7Client::loadJsonSchema(const std::string& t_path) {
    conn_->loadJsonSchema(t_path);
    if (p_g_as_engine) {
        sgrn::scripting::ScriptHost host(p_g_as_engine);
        registerSchemaTypes(host, conn_->schema_);
        registerSchemaFactories_(p_g_as_engine);
    }
}

void ScriptS7Client::registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name) {
    conn_->registerDb(t_num, t_size, t_name);
    if (p_g_as_engine) {
        sgrn::scripting::ScriptHost host(p_g_as_engine);
        registerSchemaTypes(host, conn_->schema_);
        registerSchemaFactories_(p_g_as_engine);
    }
}

void ScriptS7Client::registerUdt(const std::string& t_name, uint32_t t_size) {
    conn_->registerUdt(t_name, t_size);
}

void ScriptS7Client::addUdtField(
    const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count) {
    conn_->addUdtField(t_udt_name, t_name, t_type_str, t_offset, t_count);
}

void ScriptS7Client::loadRegistry(const std::string& t_path) {
    conn_->loadRegistry(t_path);
}

bool ScriptS7Client::hasSchema() const {
    return !conn_->schema_.dbs().empty();
}

bool ScriptS7Client::hasRegistry() const {
    return conn_->tag_table_ != nullptr;
}

void ScriptS7Client::setConnectionType(uint16_t t_type) {
    conn_->setConnectionSettings(t_type, conn_->conn_port_, true);
}

void ScriptS7Client::setPort(uint16_t t_port) {
    conn_->setConnectionSettings(conn_->conn_type_, t_port, true);
}

int ScriptS7Client::connectionType() const {
    return conn_->conn_type_;
}

uint16_t ScriptS7Client::getPort() const {
    return conn_->conn_port_;
}

} // namespace sgrn::s7shell::shell
