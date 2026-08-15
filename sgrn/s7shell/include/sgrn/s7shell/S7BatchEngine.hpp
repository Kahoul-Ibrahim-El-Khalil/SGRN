#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// S7BatchEngine.hpp  –  PDU-Aware Atomic Batch Engine
//
// PROBLEM:
// The Siemens S7 communication protocol enforces a strict Protocol Data Unit
// (PDU) size limit. When a SCADA system or Gateway needs to read or write
// multiple disparate tags (scattered across memory), a single request might
// exceed the PDU limit. Traditionally, developers must manually chunk these
// requests, calculating byte payloads to avoid rejection by the PLC.
//
// UTILITY & VALUE ADDED:
// S7BatchEngine solves this by providing a fluent, transactional API for
// queuing tag operations. It collects paths and values in memory, then
// flushes them in a minimal number of S7 `MultiVars` network calls.
//
// - It transparently enforces the Snap7 safe limit of 19 items per PDU.
// - Callers never need to chunk manually or calculate payload sizes.
// - Failures in later chunks do not rollback successful prior chunks
//   (respecting the "fire-and-forget" nature of edge SCADA telemetry).
//
// Example:
//   engine.path("Speed").write(1500.0)
//         .path("Enable").write(true)
//         .path("Setpoint").write(24.5)
//         .put(client);   // ← Chunks automatically and executes over the wire
//
// Thread-safety: A single engine instance must not be shared across threads.
// ─────────────────────────────────────────────────────────────────────────────
#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/s7.hpp>
#include <string>
#include <variant>
#include <vector>

namespace sgrn::s7shell
{

using ::sgrn::scl::Error;
using ::sgrn::scl::SchemaCode;

using sgrn::gateway::wrappers::s7::S7Client;
using sgrn::gateway::wrappers::s7::S7DataItem;

// ── ARCHITECTURE NOTE: Eliminating Runtime Polymorphism ─────────
// S7BatchEngine is now a template class instead of relying on the virtual
// IS7MemoryProvider interface. This eliminates vtable lookups (runtime
// polymorphism) in the hot path of PLC memory access.
template <typename Provider>
class S7BatchEngine {
public:
    // The S7 PDU can carry at most 19 variable items per transaction.
    static constexpr int kMaxPduItems = 19;

    explicit S7BatchEngine(Provider& t_provider);
    ~S7BatchEngine() = default;

    S7BatchEngine(const S7BatchEngine&) = delete;
    S7BatchEngine& operator=(const S7BatchEngine&) = delete;

    // ── Fluent builder ───────────────────────────────────────────────────────

    /// Append a path to the pending list.  Returns *this for chaining.
    S7BatchEngine& path(const std::string& t_p);

    /// Attach a JSON-encoded value to the most-recently added path (for put).
    S7BatchEngine& write(const std::string& t_json_val);

    /// Authoritative native overloads — bypasses JSON string roundtrip.
    S7BatchEngine& write(double t_val);
    S7BatchEngine& write(int32_t t_val);
    S7BatchEngine& write(bool t_val);

    // ── Shadow-buffer read (no network) ─────────────────────────────────────

    /// Read the value of the last path() from the shadow buffer.
    sgrn::Result<std::string, ::sgrn::scl::Error> read() const;

    /// Return JSON object of all pending paths and their shadow-buffer values.
    sgrn::Result<std::string, ::sgrn::scl::Error> toJson() const;

    // ── Network operations ───────────────────────────────────────────────────

    /// Flush pending paths → shadow buffer → PLC.
    /// Chunks into ⌈N/19⌉ writeMultiVars calls.
    sgrn::Result<void, ::sgrn::scl::Error> put(S7Client& t_client);

    /// PLC → shadow buffer (local cache).
    sgrn::Result<void, ::sgrn::scl::Error> commitLocal();

    /// PLC → shadow buffer.  Chunks into ⌈N/19⌉ readMultiVars calls.
    sgrn::Result<void, ::sgrn::scl::Error> get(S7Client& t_client);

    // ── State management ─────────────────────────────────────────────────────

    /// Clear all pending paths and values, ready for reuse.
    void reset();

    size_t pendingCount() const noexcept {
        return pending_paths_.size();
    }
    bool empty() const noexcept {
        return pending_paths_.empty();
    }

    bool hasError() const noexcept {
        return last_error_.has_value();
    }
    const Error& getLastError() const {
        return *last_error_;
    }

private:
    Provider& provider_;
    std::vector<std::string> pending_paths_;
    std::vector<std::variant<std::string, s7codec::DecodedValue>> pending_values_;
    std::optional<Error> last_error_;

    void setError_(Error t_err);
    sgrn::Result<void, ::sgrn::scl::Error> executePut_(S7Client& t_client);
    sgrn::Result<void, ::sgrn::scl::Error> executeGet_(S7Client& t_client);
};

using ::sgrn::scl::SchemaCode;
using S7Error = ::sgrn::gateway::wrappers::s7::S7Error;

inline ::sgrn::scl::Error proto_bridge(const S7Error& t_e) noexcept {
    return ::sgrn::scl::Error{SchemaCode::Generic, t_e.string()};
}

template <typename Provider>
inline S7BatchEngine<Provider>::S7BatchEngine(Provider& t_provider)
    : provider_(t_provider) {
}
template <typename Provider>
inline void S7BatchEngine<Provider>::setError_(Error t_err) {
    if (!last_error_.has_value())
        last_error_ = std::move(t_err);
}

// path appends a symbolic path/address to the current batch.
// Uses an error latching pattern: if a previous operation in the builder chain
// encountered an error, subsequent builder calls are turned into no-ops.
template <typename Provider>
inline S7BatchEngine<Provider>& S7BatchEngine<Provider>::path(const std::string& t_p) {
    if (last_error_.has_value())
        return *this;
    pending_paths_.push_back(t_p);
    pending_values_.emplace_back(); // Align indices of paths and value variants
    return *this;
}

// write binds a JSON string value to the most recently added path.
template <typename Provider>
inline S7BatchEngine<Provider>& S7BatchEngine<Provider>::write(const std::string& t_json_val) {
    if (last_error_.has_value())
        return *this;
    if (pending_paths_.empty()) {
        setError_(Error(SchemaCode::Generic, "S7BatchEngine::write() called before path()"));
        return *this;
    }
    pending_values_.back() = t_json_val;
    return *this;
}

// write overload for float/double values (prevents double -> string -> double conversion overhead)
template <typename Provider>
inline S7BatchEngine<Provider>& S7BatchEngine<Provider>::write(double t_val) {
    if (last_error_.has_value())
        return *this;
    if (pending_paths_.empty()) {
        setError_(Error(SchemaCode::Generic, "S7BatchEngine::write() called before path()"));
        return *this;
    }
    pending_values_.back() = s7codec::DecodedValue::makeDouble(t_val);
    return *this;
}

// write overload for 32-bit signed integers
template <typename Provider>
inline S7BatchEngine<Provider>& S7BatchEngine<Provider>::write(int32_t t_val) {
    if (last_error_.has_value())
        return *this;
    if (pending_paths_.empty()) {
        setError_(Error(SchemaCode::Generic, "S7BatchEngine::write() called before path()"));
        return *this;
    }
    pending_values_.back() = s7codec::DecodedValue::makeUnsigned(t_val);
    return *this;
}

// write overload for booleans
template <typename Provider>
inline S7BatchEngine<Provider>& S7BatchEngine<Provider>::write(bool t_val) {
    if (last_error_.has_value())
        return *this;
    if (pending_paths_.empty()) {
        setError_(Error(SchemaCode::Generic, "S7BatchEngine::write() called before path()"));
        return *this;
    }
    pending_values_.back() = s7codec::DecodedValue::makeBool(t_val);
    return *this;
}
template <typename Provider>
inline sgrn::Result<std::string, ::sgrn::scl::Error> S7BatchEngine<Provider>::read() const {
    if (last_error_.has_value())
        return sgrn::Result<std::string, ::sgrn::scl::Error>::Error(*last_error_);
    if (pending_paths_.empty())
        return sgrn::Result<std::string, ::sgrn::scl::Error>::Error(
            ::sgrn::scl::Error{SchemaCode::Generic, "S7BatchEngine::read() called before path()"});
    auto raw = provider_.read(pending_paths_.back());
    if (raw.hasError())
        return std::unexpected(proto_bridge(raw.error()));
    return std::move(raw).value();
}
template <typename Provider>
inline sgrn::Result<std::string, ::sgrn::scl::Error> S7BatchEngine<Provider>::toJson() const {
    if (last_error_.has_value())
        return sgrn::Result<std::string, ::sgrn::scl::Error>::Error(*last_error_);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();
    for (const auto& t_p : pending_paths_) {
        writer.Key(t_p.c_str());
        auto val_res = provider_.read(t_p);
        if (val_res.hasError())
            return std::unexpected(proto_bridge(val_res.error()));
        rapidjson::Document d;
        d.Parse(val_res.value().c_str());
        d.Accept(writer);
    }
    writer.EndObject();
    return std::string(sb.GetString());
}
template <typename Provider>
inline sgrn::Result<void, ::sgrn::scl::Error> S7BatchEngine<Provider>::put(S7Client& t_client) {
    if (last_error_.has_value())
        return sgrn::Result<void, ::sgrn::scl::Error>::Error(*last_error_);
    if (pending_paths_.empty())
        return {};
    return executePut_(t_client);
}
template <typename Provider>
inline sgrn::Result<void, ::sgrn::scl::Error> S7BatchEngine<Provider>::commitLocal() {
    if (last_error_.has_value())
        return sgrn::Result<void, ::sgrn::scl::Error>::Error(*last_error_);
    if (pending_paths_.empty())
        return {};
    auto res = provider_.commitLocalPut(pending_paths_, pending_values_);
    if (res.hasError())
        return std::unexpected(proto_bridge(res.error()));
    return {};
}
template <typename Provider>
inline sgrn::Result<void, ::sgrn::scl::Error> S7BatchEngine<Provider>::get(S7Client& t_client) {
    if (last_error_.has_value())
        return sgrn::Result<void, ::sgrn::scl::Error>::Error(*last_error_);
    if (pending_paths_.empty())
        return {};
    return executeGet_(t_client);
}
template <typename Provider>
inline void S7BatchEngine<Provider>::reset() {
    pending_paths_.clear();
    pending_values_.clear();
    last_error_.reset();
}

// executePut_ constructs the Snap7 raw item payloads and writes them to the PLC.
// It chunks the writes into maximum 19-item packages to respect S7 PDU limitations.
template <typename Provider>
inline sgrn::Result<void, ::sgrn::scl::Error> S7BatchEngine<Provider>::executePut_(S7Client& t_client) {
    std::vector<S7DataItem> all_items;
    std::vector<std::vector<uint8_t>> all_bufs;
    auto build_res = provider_.buildPutItems(pending_paths_, pending_values_, all_items, all_bufs);
    if (build_res.hasError())
        return std::unexpected(proto_bridge(build_res.error()));

    // Snap7 documented safe maximum for readMultiVars/writeMultiVars is 19 items per call.
    // Using a dynamic PDU-size based formula often produces incorrect batch boundaries
    // when dealing with heterogeneous structural payloads. A hard threshold of 19 is simple and safe.
    const size_t items_per_pdu = kMaxPduItems;

    const size_t total = all_items.size();
    size_t offset = 0;
    int chunk_idx = 0;

    // Transmit to PLC in chunks, committing each chunk's successful items to the
    // local shadow memory as soon as it lands. A later chunk failing at the
    // transport level must not discard writes the PLC already accepted.
    while (offset < total) {
        const size_t count = std::min(items_per_pdu, total - offset);
        auto rc = t_client.writeMultiVars(all_items.data() + offset, static_cast<int>(count));

        if (!rc.hasError()) {
            std::vector<std::string> ok_paths;
            std::vector<std::variant<std::string, s7codec::DecodedValue>> ok_values;
            for (size_t i = offset; i < offset + count; ++i) {
                if (all_items[i].Result == 0) {
                    ok_paths.push_back(pending_paths_[i]);
                    ok_values.push_back(pending_values_[i]);
                } else {
                    fmt::print(stderr, fg(fmt::color::red), "[S7] PUT rejected by PLC: path='{}' item_result=0x{:04X}\n", pending_paths_[i],
                        static_cast<unsigned>(all_items[i].Result));
                }
            }
            if (!ok_paths.empty()) {
                auto ok_res = provider_.commitLocalPut(ok_paths, ok_values);
                if (ok_res.hasError())
                    return std::unexpected(proto_bridge(ok_res.error()));
            }
        }

        if (rc.hasError()) {
            return sgrn::Result<void, ::sgrn::scl::Error>::Error(::sgrn::scl::Error{
                SchemaCode::Generic, fmt::format("S7BatchEngine: writeMultiVars chunk {} failed: {}", chunk_idx, rc.error().string())});
        }

        offset += count;
        chunk_idx += 1;
    }

    return {};
}
// executeGet_ constructs read items and reads them from the PLC.
// Like executePut_, it chunks requests into transactions of 19 items max.
template <typename Provider>
inline sgrn::Result<void, ::sgrn::scl::Error> S7BatchEngine<Provider>::executeGet_(S7Client& t_client) {
    std::vector<S7DataItem> all_items;
    std::vector<std::vector<uint8_t>> all_bufs;
    auto build_res = provider_.buildGetItems(pending_paths_, all_items, all_bufs);
    if (build_res.hasError())
        return std::unexpected(proto_bridge(build_res.error()));

    const size_t items_per_pdu = kMaxPduItems;

    const size_t total = all_items.size();
    size_t offset = 0;
    int chunk_idx = 0;
    bool any_item_error = false;

    // Transmit read requests to PLC, committing each chunk's results to the
    // shadow memory as soon as it lands. A later chunk failing at the
    // transport level must not discard reads the PLC already returned.
    while (offset < total) {
        const size_t count = std::min(items_per_pdu, total - offset);
        auto rc = t_client.readMultiVars(all_items.data() + offset, static_cast<int>(count));

        if (!rc.hasError()) {
            std::vector<std::string> chunk_paths(pending_paths_.begin() + offset, pending_paths_.begin() + offset + count);
            std::vector<S7DataItem> chunk_items(all_items.begin() + offset, all_items.begin() + offset + count);

            for (size_t i = 0; i < chunk_items.size(); ++i) {
                if (chunk_items[i].Result != 0) {
                    fmt::print(stderr, fg(fmt::color::red), "[S7] GET rejected by PLC: path='{}' item_result=0x{:04X}\n", chunk_paths[i],
                        static_cast<unsigned>(chunk_items[i].Result));
                    any_item_error = true;
                }
            }
            provider_.commitGetResults(chunk_paths, chunk_items);
        }

        if (rc.hasError()) {
            return sgrn::Result<void, ::sgrn::scl::Error>::Error(::sgrn::scl::Error{
                SchemaCode::Generic, fmt::format("S7BatchEngine: readMultiVars chunk {} failed: {}", chunk_idx, rc.error().string())});
        }

        offset += count;
        chunk_idx += 1;
    }

    if (any_item_error)
        return sgrn::Result<void, ::sgrn::scl::Error>::Error(
            ::sgrn::scl::Error{SchemaCode::Generic, "S7BatchEngine: one or more GET items rejected by PLC (see above)"});
    return {};
}
} // namespace sgrn::s7shell
