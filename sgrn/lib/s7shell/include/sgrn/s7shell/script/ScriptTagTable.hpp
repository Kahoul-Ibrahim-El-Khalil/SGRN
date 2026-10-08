#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/s7shell/script/AngelScriptObject.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/wrappers/s7/error.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/plcsim/PlcTagTable.hpp>
#include <sgrn/s7shell/S7BatchEngine.hpp>
#include <variant>

namespace sgrn::s7shell
{
}

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;
struct ScriptDtl;
class S7PathBatch;

using sgrn::gateway::twin::DbIOProvider;
using sgrn::plcsim::PlcTagTable;
using sgrn::s7shell::S7BatchEngine;

using S7BatchEngineForDbIo = S7BatchEngine<DbIOProvider>;
using S7BatchEngineForTagTable = S7BatchEngine<PlcTagTable>;

using S7BatchEngineForDbIoUPtr = std::unique_ptr<S7BatchEngineForDbIo>;
using S7BatchEngineForTagTableUPtr = std::unique_ptr<S7BatchEngineForTagTable>;

using EngineVariant = std::variant<S7BatchEngineForDbIoUPtr, S7BatchEngineForTagTableUPtr>;

class ScriptTagTable : public AngelScriptObject {
    friend class S7PathBatch;

public:
    explicit ScriptTagTable(ScriptS7Connection* tp_conn);
    ~ScriptTagTable() override;

    std::string get(const std::string& t_path);
    double getReal(const std::string& t_path);
    int32_t getInt(const std::string& t_path);
    bool getBool(const std::string& t_path);

    void put(const std::string& t_path, const std::string& t_raw_val);
    void put(const std::string& t_path, double t_val);
    void put(const std::string& t_path, int32_t t_val);
    void put(const std::string& t_path, bool t_val);

    void put(); // Flush batch and push dirty tags

    void get();
    S7PathBatch* getPath(const std::string& t_p);

    // ── SclError introspection (updated by every get/put) ──────────────
    bool getLastOpOk() const {
        return last_op_ok_;
    }
    /// Script-safe string form (the AS binding declares a string return —
    /// binding the enum getter directly would corrupt the call frame).
    std::string lastOpErrorStr() const {
        return std::string(::sgrn::wrappers::s7::toString(last_op_err_));
    }

    // ── Trip timing (wall-clock DTL strings, updated by every get/put) ──
    // Request stamped at method entry, response when the trip completes
    // (success or fail). Empty until the first get/put. Uses real time,
    // never the sim PLC clock.
    void stampRequest();
    void stampResponse();
    ScriptDtl* lastRequestTime() const;
    ScriptDtl* lastResponseTime() const;

    // ── Retry variants ───────────────────────────────────────────────
    std::string getRetry(const std::string& t_path, int t_max_retries = 3);
    bool putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries = 3);
    bool putRetryDouble(const std::string& t_path, double t_val, int t_max_retries = 3);
    bool putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries = 3);
    bool putRetryBool(const std::string& t_path, bool t_val, int t_max_retries = 3);

private:
    ScriptS7Connection* conn_{nullptr};
    using EngineVariant = std::variant<std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::gateway::twin::DbIOProvider>>,
        std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::plcsim::PlcTagTable>>>;
    EngineVariant engine_;

    // Last-operation error state (cleared on success, set on failure)
    bool last_op_ok_{true};
    sgrn::wrappers::s7::S7Error last_op_err_;
    // Last trip wall-clock stamps (raw DTL strings, empty until first op)
    std::string last_req_dtl_;
    std::string last_resp_dtl_;

    void notifyConnError(::sgrn::scl::SclError t_err);
    void notifyConnError(::sgrn::wrappers::s7::S7Error t_err);

    /// Fail-closed staleness for served-shadow reads/writes on dropped links:
    /// value is served best-effort but lastOpOk() goes false with NotConnected
    /// so the failed trip stays visible. Virtual (never-connected) use and
    /// online success are untouched. Call after successful offline ops.
    /// (Defined in ScriptTagTable.cpp — needs the full connection type.)
    void markStaleIfDropped();

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, SclError>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = ::sgrn::wrappers::s7::fromSclErrorToS7Error(t_r.error());
            notifyConnError(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, ::sgrn::wrappers::s7::S7Error>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = t_r.error();
            notifyConnError(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }
}; // class ScriptTagTable

} // namespace sgrn::s7shell::shell
