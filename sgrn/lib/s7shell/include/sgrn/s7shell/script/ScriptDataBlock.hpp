#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/twin/DbIoError.hpp>
#include <sgrn/s7shell/script/AngelScriptObject.hpp>
#include <sgrn/scl/errors.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/wrappers/s7/error.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace sgrn::gateway::twin
{
class DbIOProvider;
}

namespace sgrn::s7shell::shell
{
using ::sgrn::scl::SclError;

struct ScriptS7Connection;
class S7PathBatch;
class ScriptFieldProxy;

struct ScriptDtl : public ::sgrn::s7shell::AngelScriptObject {
    /// Raw timestamp string e.g. "2026-06-21 22:00:00.000000000"
    /// NOT JSON-encoded — no surrounding quotes stored here.
    std::string timestamp_str_;
    /// Returns the timestamp wrapped in JSON quotes for serialization.
    std::string toString() const {
        return "\"" + timestamp_str_ + "\"";
    }
    ScriptDtl& operator=(const ScriptDtl& t_other) {
        if (this != &t_other)
            timestamp_str_ = t_other.timestamp_str_;
        return *this;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ScriptDataBlock: Digital Twin Shadow Memory & Proxy
//
// PROBLEM:
// Direct, synchronous PLC reads/writes for every property access in a script
// or HTTP request introduces massive latency and risks overwhelming the PLC
// communications processor.
//
// UTILITY & VALUE ADDED:
// ScriptDataBlock acts as a high-performance "Shadow Memory" proxy.
// 1. It pulls a snapshot of the raw PLC Data Block into a local memory arena.
// 2. AngelScript logic reads/writes from this zero-latency local buffer.
// 3. Modifying fields locally automatically computes byte-level diffs and flags
//    "dirty regions".
// 4. A single `push()` or `put()` syncs only the dirty byte ranges back over
//    the network to the PLC, minimizing PDU footprint and PLC cycle time impact.
// ─────────────────────────────────────────────────────────────────────────────
class ScriptDataBlock : public ::sgrn::s7shell::AngelScriptObject {
    friend class S7PathBatch;

public:
    ScriptDataBlock(ScriptS7Connection* tp_conn, uint16_t t_db_num);
    ~ScriptDataBlock() override;

    // ── Read field values ────────────────────────────────────────────────
    std::string val(const std::string& t_path);

    std::string get(const std::string& t_path);
    s7codec::DecodedValue readScalar(const std::string& t_path);
    double getReal(const std::string& t_path);
    int32_t getInt(const std::string& t_path);
    bool getBool(const std::string& t_path);

    // ── Write to local cache (staged) ────────────────────────────────────
    void writeScalar(const std::string& t_path, const s7codec::DecodedValue& t_val);
    void writeDouble(const std::string& t_path, double t_val);
    void writeInt(const std::string& t_path, int32_t t_val);
    void writeBool(const std::string& t_path, bool t_val);
    /// Stage an AngelScript array/dictionary into the field at t_path.
    /// C++-only helpers for FieldProxy structured assignment — encode
    /// directly from script objects to field bytes (no JSON round-trip),
    /// same rules as put(path, json) but without the network trip. Struct
    /// dicts merge over existing bytes (missing members preserved); static
    /// arrays need exactly count elements.
    void writeScriptArray(const std::string& t_path, void* tp_arr);
    void writeScriptDict(const std::string& t_path, void* tp_dict);

    /// Staging buffer for one structured write (resolved field + pre-read
    /// bytes for merging + output buffer).
    struct StagedFieldWrite {
        ::sgrn::scl::DbField field{};
        size_t abs_offset{0};
        std::vector<uint8_t> before;
        std::vector<uint8_t> buf;
    };
    /// Resolve + pre-read; false after throwing. Commit writes the buffer,
    /// refreshes the snapshot and marks dirty bytes.
    bool stageFieldWrite(const std::string& t_path, StagedFieldWrite& t_out);
    void commitFieldWrite(const std::string& t_path, StagedFieldWrite& t_staged);

    // ── Sync to/from PLC ─────────────────────────────────────────────────
    void put(); // flush dirty segments to PLC
    void put(const std::string& t_path, const std::string& t_json_val);
    void putDouble(const std::string& t_path, double t_val);
    void putInt(const std::string& t_path, int32_t t_val);
    void putBool(const std::string& t_path, bool t_val);

    void writeDtl(const std::string& t_path, ScriptDtl* tp_dtl_obj);
    void putDtl(const std::string& t_path, ScriptDtl* tp_dtl_obj);

    // ── S7-semantic DB operations ────────────────────────────────────────
    ScriptDataBlock* get();              // read entire DB from PLC into buffer; returns self for chaining
    ScriptDataBlock* get(size_t t_size); // read specific size; returns self for chaining
    void push();                         ///< Internal — write dirty buffer segments to PLC. Use put() from scripts.

    // ── SclError introspection (updated by every get/put) ──────────────
    bool getLastOpOk() const {
        return last_op_ok_;
    }
    /// Script-safe string form (the AS binding declares a string return —
    /// binding the enum getter directly would corrupt the call frame).
    std::string lastOpErrorStr() const {
        return std::string(::sgrn::gateway::twin::toString(last_op_err_));
    }

    // ── Trip timing (wall-clock DTL strings, updated by every get/put) ──
    // Request stamped at method entry, response when the trip completes
    // (success or fail — failure latency is visible too). Empty until the
    // first get/put. Uses real time, never the sim PLC clock.
    void stampRequest();
    void stampResponse();
    ScriptDtl* lastRequestTime() const;
    ScriptDtl* lastResponseTime() const;

    // ── Retry variants ───────────────────────────────────────────────
    /// Read field from PLC, retrying up to maxRetries times on failure.
    std::string getRetry(const std::string& t_path, int t_max_retries = 3);
    /// Write field to PLC, retrying up to maxRetries times. Returns true on success.
    bool putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries = 3);
    bool putRetryDouble(const std::string& t_path, double t_val, int t_max_retries = 3);
    bool putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries = 3);
    bool putRetryBool(const std::string& t_path, bool t_val, int t_max_retries = 3);

    // ── Inspection ───────────────────────────────────────────────────────
    std::string getDbName() const;
    std::string toJson() const;
    std::string diff() const;
    void print() const; // print toJson() to stdout

    // ── Schema / registration ────────────────────────────────────────────
    void registerSize(size_t t_size);
    void addField(const std::string& t_name, const std::string& t_type, uint32_t t_offset, uint16_t t_count = 1);

    // ── Typed proxy access: db["fieldName"] → ScriptFieldProxy ───────────
    ScriptFieldProxy* opIndex(const std::string& t_key);

    S7PathBatch* getPath(const std::string& t_p);

    // ── Buffer access for typed VM (schema-driven property getters/setters)
    uint8_t* getBufferDataPointer() {
        return snapshot_buffer_.data();
    }
    const uint8_t* getBufferDataPointer() const {
        return snapshot_buffer_.data();
    }
    size_t getBufferSize() const {
        return snapshot_buffer_.size();
    }
    uint16_t getDbNumber() const {
        return db_num_;
    }
    /// Write modified bytes from buffer back to memory layer (for dirty tracking)
    void writeFieldToMemory(size_t t_offset, const uint8_t* tp_data, size_t t_len);
    /// Read bytes directly from the memory layer (for live properties)
    void readFieldFromMemory(size_t t_offset, uint8_t* tp_data, size_t t_len) const;

private:
    friend class ScriptFieldProxy;
    ScriptS7Connection* conn_{nullptr};
    uint16_t db_num_{0};
    size_t db_size_{0};
    std::vector<uint8_t> snapshot_buffer_;
    std::unique_ptr<::sgrn::gateway::twin::DbIOProvider> provider_;
    mutable bool snapshot_valid_{false};

    // Last-operation error state (cleared on success, set on failure)
    bool last_op_ok_{true};
    sgrn::gateway::twin::DbIoError last_op_err_;
    // Last trip wall-clock stamps (raw DTL strings, empty until first op)
    std::string last_req_dtl_;
    std::string last_resp_dtl_;

    void notifyConnError(const ::sgrn::scl::SclError& t_err);
    void notifyConnError(const ::sgrn::wrappers::s7::S7Error& t_err);

    /// Shared post-write bookkeeping for put(): snapshot/baseline write-back
    /// plus runtime dirty-ledger mark (gateway publish observes it).
    void commitBaseline(const std::string& t_path, const std::string& t_json_val);

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, SclError>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = ::sgrn::wrappers::s7::fromSclErrorToS7Error(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }

    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, ::sgrn::wrappers::s7::S7Error>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = gateway::twin::fromS7ErrorToDbIoError(t_r.error());
            return false;
        }
        last_op_ok_ = true;
        return true;
    }
    template <typename T>
    bool setOpResult(const ::sgrn::Result<T, ::sgrn::gateway::twin::DbIoError>& t_r) {
        if (t_r.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = t_r.error();
            return false;
        }
        last_op_ok_ = true;
        return true;
    }
}; // class ScriptDataBlock

} // namespace sgrn::s7shell::shell
