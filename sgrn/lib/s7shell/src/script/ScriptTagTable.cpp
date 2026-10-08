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
#include <stdexcept>
#include <string>

#include <chrono>
#include <thread>

namespace sgrn::s7shell::shell
{

using sgrn::scl::SclError;
using sgrn::wrappers::s7::S7Error;
ScriptTagTable::ScriptTagTable(ScriptS7Connection* tp_conn)
    : conn_(tp_conn)
    , engine_(
          conn_->tag_table_ ? std::make_unique<::sgrn::s7shell::S7BatchEngine<::sgrn::plcsim::PlcTagTable>>(*conn_->tag_table_) : nullptr) {
}

ScriptTagTable::~ScriptTagTable() = default;

void ScriptTagTable::notifyConnError(SclError t_err) {
    if (conn_)
        conn_->setLastError(t_err);
}

void ScriptTagTable::notifyConnError(S7Error t_err) {
    if (conn_)
        conn_->setLastError(t_err);
}

void ScriptTagTable::addRef() {
    ++ref_count_;
}

void ScriptTagTable::markStaleIfDropped() {
    if (conn_ && !conn_->client_.isConnected() && conn_->wasConnected()) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::NotConnected;
        conn_->setLastError(S7Error::NotConnected);
    }
}

void ScriptTagTable::release() {
    if (--ref_count_ == 0)
        delete this;
}

// get performs an immediate, synchronous network read from the PLC for a single tag,
// updates the shadow cache, and returns the retrieved value.
std::string ScriptTagTable::get(const std::string& t_path) {
    if (conn_->hasRuntimeTag(t_path)) {
        auto res = conn_->runtimeTagGet(t_path);
        setOpResult(res);
        if (!res.hasError())
            markStaleIfDropped();
        return res.value_or("null");
    }
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return "null";
    }
    if (!conn_->client_.isConnected()) {
        // Offline: serve the file-table shadow cache instead of failing
        // NotConnected.
        auto res = conn_->tag_table_->read(t_path);
        if (res.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = res.error();
            return "null";
        }
        last_op_ok_ = true;
        markStaleIfDropped();
        return res.value();
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
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    if (conn_->hasRuntimeTag(t_path)) {
        auto res = conn_->runtimeTagPut(t_path, t_json_val);
        setOpResult(res);
        if (!res.hasError())
            markStaleIfDropped();
        return;
    }
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return;
    }
    auto res = conn_->tag_table_->put(conn_->client_, t_path, t_json_val);
    setOpResult(res);
}

// put (double overload) performs an immediate, synchronous write of a float/double tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, double t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    if (conn_->hasRuntimeTag(t_path)) {
        auto res = conn_->runtimeTagPut(t_path, sb.GetString());
        setOpResult(res);
        if (!res.hasError())
            markStaleIfDropped();
        return;
    }
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return;
    }
    auto res = conn_->tag_table_->put(conn_->client_, t_path, sb.GetString());
    setOpResult(res);
}

// put (int32 overload) performs an immediate, synchronous write of an integer tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, int32_t t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    if (conn_->hasRuntimeTag(t_path)) {
        auto res = conn_->runtimeTagPut(t_path, sb.GetString());
        setOpResult(res);
        if (!res.hasError())
            markStaleIfDropped();
        return;
    }
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return;
    }
    auto res = conn_->tag_table_->put(conn_->client_, t_path, sb.GetString());
    setOpResult(res);
}

// put (bool overload) performs an immediate, synchronous write of a boolean tag to the PLC.
void ScriptTagTable::put(const std::string& t_path, bool t_val) {
    if (conn_->hasRuntimeTag(t_path)) {
        auto res = conn_->runtimeTagPut(t_path, t_val ? "true" : "false");
        setOpResult(res);
        if (!res.hasError())
            markStaleIfDropped();
        return;
    }
    if (!conn_->tag_table_) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return;
    }
    auto res = conn_->tag_table_->put(conn_->client_, t_path, t_val ? "true" : "false");
    setOpResult(res);
}

void ScriptTagTable::put() {
    sgrn::Result<void, SclError> res = {};
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
    // Staged runtime tags were applied to the shared backing by write(); push
    // the dirty ones onto the wire when connected (no ledger consume).
    if (!res.hasError()) {
        if (conn_->pushRuntimeTags().hasError())
            res = SclError::Generic;
    }
    setOpResult(res);
}

// get performs a complete pull of all registered symbolic tags from the PLC.
void ScriptTagTable::get() {
    const bool has_file_tags = conn_->tag_table_ != nullptr;
    const bool has_rt_tags = conn_->runtime_ && !conn_->runtime_->tagNames().empty();
    if (!has_file_tags && !has_rt_tags) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return;
    }
    sgrn::Result<void, SclError> res = {};
    if (has_file_tags) {
        auto r = conn_->tag_table_->pullAll(conn_->client_);
        if (r.hasError())
            res = r.error();
    }
    // Refresh runtime tags (wire read when connected, arena re-read after
    // commit; marks dirty on change like the DB get-diff path).
    for (const auto& name : conn_->runtime_->tagNames()) {
        if (conn_->runtimeTagGet(name).hasError() && !res.hasError())
            res = SclError::Generic;
    }
    if (!res.hasError())
        markStaleIfDropped();
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
    if (!conn_->tag_table_ && !conn_->hasRuntimeTag(t_path)) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return "null";
    }
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        sgrn::Result<std::string, S7Error> res = conn_->hasRuntimeTag(t_path)
                                                     ? conn_->runtimeTagGet(t_path)
                                                     : (conn_->tag_table_ ? conn_->tag_table_->get(conn_->client_, t_path)
                                                                          : sgrn::Result<std::string, S7Error>(S7Error::ReadError));
        if (!res.hasError()) {
            last_op_ok_ = true;
            markStaleIfDropped();
            return res.value();
        }
        last_op_ok_ = false;
        last_op_err_ = res.error();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] TagTable.getRetry('{}') attempt {}/{} failed: {}\n", t_path, attempt,
            t_max_retries, toString(res.error()));
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return "null";
}

bool ScriptTagTable::putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    if (!conn_->tag_table_ && !conn_->hasRuntimeTag(t_path)) {
        last_op_ok_ = false;
        last_op_err_ = S7Error::ReadError;
        return false;
    }
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        sgrn::Result<void, S7Error> res = conn_->hasRuntimeTag(t_path)
                                              ? conn_->runtimeTagPut(t_path, t_json_val)
                                              : (conn_->tag_table_ ? conn_->tag_table_->put(conn_->client_, t_path, t_json_val)
                                                                   : sgrn::Result<void, S7Error>(S7Error::WriteError));
        if (!res.hasError()) {
            last_op_ok_ = true;
            markStaleIfDropped();
            return true;
        }
        last_op_ok_ = false;
        last_op_err_ = res.error();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] TagTable.putRetry('{}') attempt {}/{} failed: {}\n", t_path, attempt,
            t_max_retries, toString(res.error()));
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool ScriptTagTable::putRetryDouble(const std::string& t_path, double t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Double(t_val);
    // Explicit std::string: bare const char* would bind the
    // putRetry(string, bool, int) overload and retry writing "true".
    return putRetry(t_path, std::string(sb.GetString()), t_max_retries);
}

bool ScriptTagTable::putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Int(t_val);
    // See putRetryDouble: explicit std::string defeats the bool overload.
    return putRetry(t_path, std::string(sb.GetString()), t_max_retries);
}

bool ScriptTagTable::putRetryBool(const std::string& t_path, bool t_val, int t_max_retries) {
    // Explicit strings (see putBool): no putRetry(string, bool, int) overload.
    return putRetry(t_path, t_val ? std::string("true") : std::string("false"), t_max_retries);
}

} // namespace sgrn::s7shell::shell
