#include <sgrn/s7shell/S7BatchEngine.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/scl/types.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>

#include <chrono>
#include <thread>

namespace sgrn::s7shell::shell
{

ScriptTagTable::ScriptTagTable(ScriptS7Connection* tp_conn)
    : conn_(tp_conn)
    , engine_(conn_->tag_table_ ? std::make_unique<::sgrn::s7shell::S7BatchEngine<::sgrn::s7shell::PlcTagTable>>(*conn_->tag_table_)
                                : nullptr) {
}

ScriptTagTable::~ScriptTagTable() = default;

void ScriptTagTable::notifyConnError(const ::sgrn::scl::Error& t_err) {
    if (conn_)
        conn_->setLastError(t_err);
}

void ScriptTagTable::notifyConnError(const ::sgrn::gateway::wrappers::s7::S7Error& t_err) {
    if (conn_)
        conn_->setLastError(t_err);
}

void ScriptTagTable::addRef() {
    ++ref_count_;
}

void ScriptTagTable::release() {
    if (--ref_count_ == 0)
        delete this;
}

// val reads a tag value from the local shadow memory cache (zero network calls).
std::string ScriptTagTable::getVal(const std::string& t_path) {
    if (!conn_->tag_table_)
        return "null";
    return shell::valueOr(conn_->tag_table_->read(t_path), std::string{"null"});
}

// setVal stages a value in the local shadow cache memory and marks it dirty,
// ready to be synchronized with the PLC during the next batch flush (zero network calls).
void ScriptTagTable::setVal(const std::string& t_path, const std::string& t_json_val) {
    if (!conn_->tag_table_)
        return;
    (void)shell::ok(conn_->tag_table_->write(t_path, t_json_val));
}

// get performs an immediate, synchronous network read from the PLC for a single tag,
// updates the shadow cache, and returns the retrieved value.
std::string ScriptTagTable::get(const std::string& t_path) {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return "null";
    }
    auto res = conn_->tag_table_->get(conn_->client_, t_path);
    setOpResult(res);
    return res.value_or("null");
}

double ScriptTagTable::getReal(const std::string& t_path) {
    return shell::jsonScalarDouble(get(t_path));
}

int32_t ScriptTagTable::getInt(const std::string& t_path) {
    return shell::jsonScalarInt(get(t_path));
}

bool ScriptTagTable::getBool(const std::string& t_path) {
    return get(t_path) == "true";
}

// put (string overload) performs an immediate, synchronous write of a single tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, const std::string& t_raw_val) {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return;
    }
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    auto res = conn_->tag_table_->put(conn_->client_, t_path, t_json_val);
    setOpResult(res);
}

// put (double overload) performs an immediate, synchronous write of a float/double tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, double t_val) {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return;
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    auto res = conn_->tag_table_->put(conn_->client_, t_path, sb.GetString());
    setOpResult(res);
}

// put (int32 overload) performs an immediate, synchronous write of an integer tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, int32_t t_val) {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return;
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    auto res = conn_->tag_table_->put(conn_->client_, t_path, sb.GetString());
    setOpResult(res);
}

// put (bool overload) performs an immediate, synchronous write of a boolean tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, bool t_val) {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return;
    }
    auto res = conn_->tag_table_->put(conn_->client_, t_path, t_val ? "true" : "false");
    setOpResult(res);
}

// write (string overload) queues a tag write in the S7BatchEngine (no network action).
// Queued operations will be sent in one network PDU-optimized chunk when put() is called.
void ScriptTagTable::write(const std::string& t_path, const std::string& t_raw_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    std::visit([&](auto& e) { e->path(t_path).write(t_json_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(
            std::visit([&](auto& e) { return e->getLastError(); }, engine_), fmt::format("tags.write('{}', '{}')", t_path, t_raw_val));
    }
}

void ScriptTagTable::write(const std::string& t_path, double t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    std::visit([&](auto& e) { e->path(t_path).write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
}

void ScriptTagTable::write(const std::string& t_path, int32_t t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    std::visit([&](auto& e) { e->path(t_path).write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
}

void ScriptTagTable::write(const std::string& t_path, bool t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    std::visit([&](auto& e) { e->path(t_path).write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
}

void ScriptTagTable::write(const std::string& t_path, CScriptDictionary* tp_dict) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    std::visit([&](auto& e) { e->path(t_path).write(shell::convertDictToJson(tp_dict)); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
}

void ScriptTagTable::write(const std::string& t_path, CScriptArray* tp_arr) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    std::visit([&](auto& e) { e->path(t_path).write(shell::convertArrayToJson(tp_arr)); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        last_op_ok_ = false;
        last_op_err_ = std::visit([&](auto& e) { return e->getLastError().string(); }, engine_);
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
}

void ScriptTagTable::put() {
    sgrn::Result<void, ::sgrn::scl::Error> res = {};
    if (std::visit([&](const auto& e) { return e != nullptr; }, engine_) && !std::visit([&](auto& e) { return e->empty(); }, engine_)) {
        res = std::visit([&](auto& e) { return e->put(conn_->client_); }, engine_);
        std::visit([&](auto& e) { e->reset(); }, engine_); // Always reset the builder engine so it can accept new batches
    }
    if (!res.hasError() && conn_->tag_table_) {
        auto res2 = conn_->tag_table_->pushDirty(conn_->client_);
        if (res2.hasError()) {
            res = res2;
        }
    }
    setOpResult(res);
}

// get performs a complete pull of all registered symbolic tags from the PLC.
void ScriptTagTable::get() {
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return;
    }
    auto res = conn_->tag_table_->pullAll(conn_->client_);
    setOpResult(res);
}

// path initializes a fluent path-based batch builder chain.
S7PathBatch* ScriptTagTable::getPath(const std::string& t_p) {
    auto* p_batch = new S7PathBatch(this);
    p_batch->path(t_p);
    return p_batch;
}

std::string ScriptTagTable::getRetry(const std::string& t_path, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return "null";
    }
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        auto res = conn_->tag_table_->get(conn_->client_, t_path);
        if (!res.hasError()) {
            last_op_ok_ = true;
            last_op_err_.clear();
            return res.value();
        }
        last_op_ok_ = false;
        last_op_err_ = res.error().string();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] TagTable.getRetry('{}') attempt {}/{} failed: {}\n", t_path, attempt,
            t_max_retries, res.error().string());
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return "null";
}

bool ScriptTagTable::putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = "Tag table not initialized";
        return false;
    }
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        auto res = conn_->tag_table_->put(conn_->client_, t_path, t_json_val);
        if (!res.hasError()) {
            last_op_ok_ = true;
            last_op_err_.clear();
            return true;
        }
        last_op_ok_ = false;
        last_op_err_ = res.error().string();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] TagTable.putRetry('{}') attempt {}/{} failed: {}\n", t_path, attempt,
            t_max_retries, res.error().string());
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool ScriptTagTable::putRetryDouble(const std::string& t_path, double t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Double(t_val);
    return putRetry(t_path, sb.GetString(), t_max_retries);
}

bool ScriptTagTable::putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Int(t_val);
    return putRetry(t_path, sb.GetString(), t_max_retries);
}

bool ScriptTagTable::putRetryBool(const std::string& t_path, bool t_val, int t_max_retries) {
    return putRetry(t_path, t_val ? "true" : "false", t_max_retries);
}

} // namespace sgrn::s7shell::shell
