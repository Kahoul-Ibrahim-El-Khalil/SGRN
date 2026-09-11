#pragma once

// NOTE: WebSocketAdapter deliberately does NOT include PlcMemory.hpp.
// All PLC data flows exclusively through TelemetryBroker (pub/sub).
// This keeps the networking layer decoupled from the PLC/memory layer.
#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/northbound/NorthboundServer.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <ankerl/unordered_dense.h>
#include <atomic>
#include <crow.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
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
 * TRANSPORT
 * ─────────
 * Crow (asio-based) serves the WebSocket endpoint at `/ws`. In the full
 * gateway the route is registered on the same NorthboundServer listener as
 * the HTTP API, so both share one port and one asio event loop. Standalone
 * start() (used by s7shell's WebSocketServer binding) brings up a private
 * listener instead.
 *
 * THREADING MODEL
 * ───────────────
 * - Crow runs its own asio pool for accept/read/write.
 * - The TelemetryBroker callback is dispatched on the global io_context
 *   and sends directly to each connected client — no queue needed.
 * - broadcast() is therefore fully non-blocking from the PLC main loop.
 * - All sends are performed while holding clients_mutex_. Crow posts sends
 *   into its io_context (non-blocking), so the close handler — which takes
 *   the same mutex before erasing — can never race with a send. A connection
 *   is only ever touched while it is registered, i.e. between onopen and
 *   onclose, which pins its lifetime for sending threads.
 *
 * SUBSCRIPTION FILTERING
 * ──────────────────────
 * Clients can send {"command":"subscribe","db":N} to limit updates to a
 * specific Data Block number. An empty subscription set means "all DBs".
 */
class WebSocketAdapter {
public:
    using BinaryReadFn = std::function<sgrn::Result<void, std::string>(uint16_t t_db, size_t t_offset, size_t t_size, uint8_t* tp_out)>;

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
        const ::sgrn::scl::PlcSchemaStore* tp_registry = nullptr, std::function<std::string()> t_full_snapshot_provider = {},
        BinaryReadFn t_binary_read_fn = {});

    /**
     * @brief Register the `/ws` route on an externally owned Crow app.
     *
     * Used by the unified gateway path together with HttpAdapter on the SAME
     * app before its listener starts, so HTTP and WebSocket share one port.
     * Bound references must be configured first via configure(). They must
     * outlive the server. Must be called before the app starts listening.
     */
    void registerRoutes(crow::SimpleApp& t_app);

    /**
     * @brief Store the bound references used by registerRoutes().
     *
     * start() calls this internally; the unified gateway path calls it
     * explicitly before registerRoutes().
     */
    void configure(SecurityManagerSptr tsp_security_manager, const ::sgrn::scl::PlcSchemaStore* tp_registry,
        std::function<std::string()> t_full_snapshot_provider, BinaryReadFn t_binary_read_fn);

    void stop();

    /// Inject a pre-built LeafDictionary for dictionary-mode live traffic.
    void setLeafDictionary(const twin::LeafDictionary& t_dict) {
        dict_ = &t_dict;
    }

    /**
     * @brief Push an arbitrary JSON snapshot to every connected client
     *        immediately. Non-blocking.
     */
    void broadcastDelta(const std::string& t_json_snapshot, uint64_t t_timestamp_ms = 0);

private:
    struct ClientContext {
        struct BinarySubscription {
            uint16_t db;
            size_t offset;
            size_t size;
        };

        std::string ip;
        std::string origin;
        std::vector<std::string> headers;
        std::set<std::string> subscriptions;
        std::vector<BinarySubscription> binary_subscriptions;
        bool dictionary_mode{false}; ///< when true, send flat id-keyed payloads
        /// Pre-resolved leaf-id ranges for dictionary-mode subscription checks.
        /// Computed once at subscribe/unsubscribe time — avoids per-event path
        /// resolution and string-based overlap detection.
        struct LeafRange {
            twin::LeafId start;
            twin::LeafId end;
        };
        std::vector<LeafRange> leaf_ranges;
    };

    struct TargetInfo {
        crow::websocket::connection* conn;
        bool needs_filter;
        bool dictionary_mode;
        std::set<std::string> field_subs;
        std::vector<ClientContext::LeafRange> leaf_ranges;
    };

    void handleTelemetryEvent(const sgrn::gateway::core::TelemetryEvent& t_event);
    std::vector<TargetInfo> collectTargets(const sgrn::gateway::core::TelemetryEvent& t_event, bool& t_any_needs_filter);
    std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<crow::websocket::connection*>> collectBinaryTargets(uint16_t t_db);
    bool sendBinaryFrame(crow::websocket::connection* tp_conn, uint16_t t_db, size_t t_offset, size_t t_size, double t_timestamp_seconds);
    void handleClientMessage(crow::websocket::connection& t_conn, const std::string& t_message);
    /// Resolve a client's string subscriptions to contiguous leaf-id ranges
    /// using the shared dictionary. Called once at subscribe/unsubscribe time.
    void resolveLeafRanges(ClientContext& t_ctx);

    // Sends while holding clients_mutex_ (see THREADING MODEL above).
    void sendText(crow::websocket::connection& t_conn, const std::string& t_payload);
    void sendBinary(crow::websocket::connection& t_conn, const void* tp_data, size_t t_size);

    // Standalone listener, used only by start(). The unified gateway path
    // registers routes on an external app instead (registerRoutes()).
    std::unique_ptr<northbound::NorthboundServer> server_;
    std::mutex clients_mutex_;
    // Set by start()/registerRoutes(), cleared by stop(). Read from arbitrary
    // sender threads (broadcastDelta), hence atomic.
    std::atomic<size_t> broker_sub_id_{0};
    ankerl::unordered_dense::map<crow::websocket::connection*, ClientContext> clients_;

    std::atomic<bool> running_{false};

    std::shared_ptr<::sgrn::gateway::SecurityManager> security_manager_;
    const ::sgrn::scl::PlcSchemaStore* registry_{nullptr};

    // Returns the full plant JSON to seed newly-connected clients with the
    // current (possibly persistence-restored) twin state. See start().
    std::function<std::string()> full_snapshot_provider_;
    BinaryReadFn binary_read_fn_;

    const twin::LeafDictionary* dict_{nullptr}; ///< shared dictionary for dictionary-mode

    /// Last known value (serialized JSON) per leaf id, fed from flat
    /// DeltaSnapshot events. Served as catch-up to dictionary-mode clients
    /// that subscribe after the fact — e.g. a dashboard opened when the twin
    /// is quiet (replay, idle plant). Bounded by the schema's leaf count.
    std::mutex last_values_mutex_;
    std::unordered_map<twin::LeafId, std::string> last_flat_values_;

    /// Record a flat {"<id>": value} payload into the last-value cache.
    void rememberFlatValues(const std::string& t_flat_json);
    /// Send cached values covered by t_ranges to one client (flat form).
    /// No-op when the cache holds nothing in those ranges.
    void sendCatchUp(crow::websocket::connection& t_conn, const std::vector<ClientContext::LeafRange>& t_ranges);
};

} // namespace sgrn::gateway::adapters::websocket
