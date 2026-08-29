#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <memory>
#include <string>

class CScriptDictionary;
class CScriptArray;

#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/s7shell/PlcTagTable.hpp>
#include <sgrn/s7shell/S7BatchEngine.hpp>
#include <variant>

namespace sgrn::s7shell
{
}

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;
class S7PathBatch;

using sgrn::gateway::twin::DbIOProvider;
using sgrn::s7shell::PlcTagTable;
using sgrn::s7shell::S7BatchEngine;

using S7BatchEngineForDbIo = S7BatchEngine<DbIOProvider>;
using S7BatchEngineForTagTable = S7BatchEngine<PlcTagTable>;

using S7BatchEngineForDbIoUPtr = std::unique_ptr<S7BatchEngineForDbIo>;
using S7BatchEngineForTagTableUPtr = std::unique_ptr<S7BatchEngineForTagTable>;

using EngineVariant = std::variant<S7BatchEngineForDbIoUPtr, S7BatchEngineForTagTableUPtr>;

class ScriptTagTable {
    friend class S7PathBatch;

public:
    explicit ScriptTagTable(ScriptS7Connection* tp_conn);
    ~ScriptTagTable();

    void addRef();
    void release();

    std::string getVal(const std::string& t_path);
    void setVal(const std::string& t_path, const std::string& t_json_val);

    std::string get(const std::string& t_path);
    double getReal(const std::string& t_path);
    int32_t getInt(const std::string& t_path);
    bool getBool(const std::string& t_path);

    void put(const std::string& t_path, const std::string& t_raw_val);
    void put(const std::string& t_path, double t_val);
    void put(const std::string& t_path, int32_t t_val);
    void put(const std::string& t_path, bool t_val);

    void write(const std::string& t_path, const std::string& t_raw_val);
    void write(const std::string& t_path, double t_val);
    void write(const std::string& t_path, int32_t t_val);
    void write(const std::string& t_path, bool t_val);
    void write(const std::string& t_path, CScriptDictionary* tp_dict);
    void write(const std::string& t_path, CScriptArray* tp_arr);
    void put(); // Flush batch and push dirty tags

    void get();
    S7PathBatch* getPath(const std::string& t_p);

    // ── SclError introspection (updated by every get/put) ──────────────
    bool getLastOpOk() const {
        return last_op_ok_;
    }
    sgrn::gateway::wrappers::s7::S7Error getLastError() const {
        return last_op_err_;
    }

    // ── Retry variants ───────────────────────────────────────────────
    std::string getRetry(const std::string& t_path, int t_max_retries = 3);
    bool putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries = 3);
    bool putRetryDouble(const std::string& t_path, double t_val, int t_max_retries = 3);
    bool putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries = 3);
    bool putRetryBool(const std::string& t_path, bool t_val, int t_max_retries = 3);

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
    using EngineVariant = std::variant<std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::gateway::twin::DbIOProvider>>,
        std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::s7shell::PlcTagTable>>>;
    EngineVariant engine_;

    // Last-operation error state (cleared on success, set on failure)
    bool last_op_ok_{true};
    sgrn::gateway::wrappers::s7::S7Error last_op_err_;

    void notifyConnError(::sgrn::scl::SclError t_err);
    void notifyConnError(::sgrn::gateway::wrappers::s7::S7Error t_err);

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, SclError>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = gateway::wrappers::s7::fromSclErrorToS7Error(t_r.error());
            notifyConnError(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, ::sgrn::gateway::wrappers::s7::S7Error>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = t_r.error();
            notifyConnError(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }
    inline bool doesEngineHaveAnError() {
        return std::visit([&](auto& e) { return e->hasError(); }, engine_);
        ;
    }
    inline S7Error getLastS7ErrorFromEngine() {
        return std::visit([&](auto& e) { return gateway::wrappers::s7::fromSclErrorToS7Error(e->getLastError()); }, engine_);
    }
    inline bool isEngineNull() {
        return std::visit([&](const auto& e) { return e == nullptr; }, engine_);
    }
}; // class ScriptTagTable

} // namespace sgrn::s7shell::shell
