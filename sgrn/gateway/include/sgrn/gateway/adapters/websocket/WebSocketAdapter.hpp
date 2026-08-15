#pragma once

// NOTE: WebSocketAdapter deliberately does NOT include PlcMemory.hpp.
// All PLC data flows exclusively through TelemetryBroker (pub/sub).
// This keeps the networking layer decoupled from the PLC/memory layer.
#include <sgrn/Result.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <ixwebsocket/IXWebSocketServer.h>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::core
{
struct TelemetryEvent;
}

namespace sgrn::gateway::adapters::websocket
{

/**
 * @brief WebSocket Facade for S7Gateway.
 *
 * Streams live delta snapshots to connected WebSocket clients.
 *
 * ARCHITECTURAL ROLE
 * ──────────────────
 * This class is a pure *northbound transport adapter*. It sits at the top
 * of the data-flow stack and has NO knowledge of the PLC memory model:
 *
 *   PlcMemory (OT layer)
 *       │  writes via writeMemory()
 *       ▼
 *   TelemetryBroker (event bus)  ◄── WebSocketAdapter subscribes here
 *       │  publishes TelemetryEvent{DeltaSnapshot}
 *       ▼
 *   WebSocketAdapter (northbound transport)
 *       │  forwards JSON frames to connected browser/app clients
 *       ▼
 *   WebSocket Clients (UI / dashboards)
 *
 * THREADING MODEL
 * ───────────────
 * - IXWebSocket manages its own internal threads for accept/read/write.
 * - The TelemetryBroker callback is dispatched on the global io_context
 *   and sends directly to each connected client — no queue needed.
 * - broadcast() is therefore fully non-blocking from the PLC main loop.
 *
 * SUBSCRIPTION FILTERING
 * ──────────────────────
 * Clients can send {"command":"subscribe","db":N} to limit updates to a
 * specific Data Block number. An empty subscription set means "all DBs".
 */
class WebSocketAdapter {
public:
    WebSocketAdapter();
    ~WebSocketAdapter();

    /**
     * @brief Start the WebSocket server and subscribe to TelemetryBroker.
     *
     * No reference to PlcMemory is taken — all data arrives via the broker.
     *
     * @param t_full_snapshot_provider Callback that returns the current full
     *        plant JSON (e.g. `getDigitalTwinJsonString()`). When set, the
     *        snapshot is pushed to every client right after the WebSocket
     *        handshake completes, *before* any DeltaSnapshot frame, so a client
     *        connecting after a gateway restart immediately receives the
     *        restored ("reclaimed") twin state instead of waiting for the next
     *        PLC delta. May be empty to keep the previous "deltas only" mode.
     */
    sgrn::Result<void> start(const std::string& t_ip, uint16_t t_port, SecurityManagerSptr tsp_security_manager,
        const ::sgrn::scl::PlcSchemaStore* tp_registry = nullptr, std::function<std::string()> t_full_snapshot_provider = {});
    void stop();

    struct Envelope {
        uint16_t db;
        std::string payload;
    };

    /**
     * @brief Receive a raw delta snapshot (e.g. {"FactoryPackagingMixer":{...}}),
     *        explode it into per-field {event,db,path,value,timestamp} envelopes
     *        and enqueue them for the worker thread.  Non-blocking.
     */
    void broadcastDelta(const std::string& t_json_snapshot, uint64_t t_timestamp_ms = 0);

private:
    struct TargetInfo {
        std::shared_ptr<ix::WebSocket> sp_ws;
        bool needs_filter;
        std::set<std::string> field_subs;
    };

    void setupConnectionHandler();
    void handleTelemetryEvent(const sgrn::gateway::core::TelemetryEvent& t_event);
    std::vector<TargetInfo> collectTargets(const sgrn::gateway::core::TelemetryEvent& t_event, bool& t_any_needs_filter);
    void workerLoop();
    void handleClientMessage(std::shared_ptr<ix::WebSocket> tsp_ws, const std::string& t_message);

    // IXWebSocket server
    std::unique_ptr<ix::WebSocketServer> server_;

    // Connected clients and their path-level subscriptions.
    // An empty set means the client receives all updates.
    struct ClientContext {
        std::string ip;
        std::string origin;
        std::vector<std::string> headers;
        std::set<std::string> subscriptions;
    };
    std::mutex clients_mutex_;
    size_t broker_sub_id_ = 0;
    std::unordered_map<std::shared_ptr<ix::WebSocket>, ClientContext> clients_;

    // Snapshot queue — enqueued by producer, dequeued by worker
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<Envelope> snapshot_queue_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::shared_ptr<::sgrn::gateway::SecurityManager> security_manager_;
    const ::sgrn::scl::PlcSchemaStore* registry_{nullptr};

    // Returns the full plant JSON to seed newly-connected clients with the
    // current (possibly persistence-restored) twin state. See start().
    std::function<std::string()> full_snapshot_provider_;
};

} // namespace sgrn::gateway::adapters::websocket
