#pragma once
// =============================================================================
// GatewayBinding — client binding for gateway dirty-tag synchronization
//
// GatewayBinding connects to a running SGRN Gateway's WebSocket endpoint and
// listens for DeltaSnapshot events. When a dirty tag event arrives, it
// decodes the relevant field paths, writes the value through its PlcRuntime
// (schema-aware field encode, via the same memory path scripts use), and
// marks the affected DB dirty on that runtime. Local dirty regions are
// published back to the Gateway over HTTP /memory/batch, because the Gateway
// WebSocket accepts only subscribe/unsubscribe commands.
//
// Data flow:
//   Gateway WebSocket (DeltaSnapshot JSON)
//       ↓  parse field path + value
//   Schema lookup (byte offset + codec) via the source client's PlcRuntime
//       ↓  encode value → PlcRuntime memory + mark dirty
//   HTTP PUT /memory/batch
//       ↓
//   Gateway twin memory
//
// GatewaySync is therefore a client binding attached to the same PlcRuntime
// as the shell's other protocol endpoints, not a direct bridge that owns
// memory or schema itself.
//
// Usage from AngelScript:
//   PlcRuntime@ rt = PlcRuntime("schema.scl");
//   GatewaySync@ sync = GatewaySync(rt);
//   sync.subscribeDb(1);   // optional: filter to DB 1
//   sync.connect("ws://192.168.1.1:8080");
//   // GatewaySync runs in background until sync.disconnect()
// =============================================================================

#include <sgrn/s7shell/runtime/PlcRuntime.hpp>
#include <atomic>
#include <condition_variable>
#include <ixwebsocket/IXWebSocket.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sgrn::s7shell::connection
{
using runtime::PlcRuntimeSPtr;
class GatewaySync {
public:
    explicit GatewaySync(PlcRuntimeSPtr tsp_runtime);
    ~GatewaySync();

    /// Optionally restrict sync to specific DB numbers.
    /// If no DBs are subscribed, all DB deltas are written.
    void subscribeDb(uint16_t t_db);
    void unsubscribeDb(uint16_t t_db);
    void publishOnDirty(bool t_enabled);

    /// Connect to the gateway WebSocket. Non-blocking — starts background thread.
    bool connect(const std::string& t_ws_url);

    /// Disconnect and stop the background receive thread.
    void disconnect();

    bool isConnected() const {
        return connected_.load();
    }
    std::string getLastError() const;

private:
    void onMessage(const ix::WebSocketMessagePtr& t_msg);
    void handleDeltaSnapshot(const std::string& t_json_payload, uint16_t t_db_hint);
    void onRuntimeDirty(uint16_t t_db, uint32_t t_offset, uint32_t t_length);
    void requestPublish();
    void publishWorkerLoop();
    bool publishDirtyBatch();

    /// Writes one "DbName.field.subfield" = raw_value pair through the runtime
    /// and marks the owning DB dirty on that runtime. Returns false and leaves
    /// an error string in `err` on failure.
    bool writeFieldThroughRuntime(const std::string& t_target, const std::string& t_raw_val, std::string& t_err);
    bool isDbSubscribed(uint16_t t_db) const;

    PlcRuntimeSPtr runtime_;
    size_t dirty_observer_id_{0};
    std::string http_base_url_;

    ix::WebSocket ws_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> publish_on_dirty_{true};
    std::atomic<bool> publishing_{false};
    static thread_local bool suppress_publish_;
    // markDirty() observers run on the writer's thread: script execution,
    // S7 server callbacks, proxy polling, or the WebSocket callback. Keep
    // that path non-blocking by waking this worker instead of issuing HTTP
    // inline. The worker debounces bursts and publishes all runtime dirty
    // regions in one /memory/batch request.
    std::thread publish_worker_;
    std::mutex publish_mutex_;
    std::condition_variable publish_cv_;
    bool publish_requested_{false};
    bool stop_publish_worker_{false};

    mutable std::mutex subs_mutex_;
    std::set<uint16_t> subscribed_dbs_; ///< empty = all

    mutable std::mutex err_mutex_;
    std::string last_error_;
};

} // namespace sgrn::s7shell::connection
