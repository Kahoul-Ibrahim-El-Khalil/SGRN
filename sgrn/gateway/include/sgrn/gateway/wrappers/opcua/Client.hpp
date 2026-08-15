#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>

#include <open62541/client.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::wrappers::opcua
{

// ── Configuration ─────────────────────────────────────────────────────────────

struct ClientConfig {
    std::string endpoint_url;
    uint32_t reconnect_delay_ms = 2000;
    bool auto_reconnect = false;

    uint32_t timeout_ms = 5000;
};

// ── Client ────────────────────────────────────────────────────────────────────

/// RAII move-only wrapper over UA_Client*.
///
/// THREAD SAFETY: This class is NOT thread-safe. All methods must be called
/// from the same thread. For concurrent access, serialise externally or
/// instantiate one Client per thread.
///
/// Typical usage:
///   auto res = Client::connect({"opc.tcp://plc:4840"});
///   if (res.hasError()) ...
///   Client client = std::move(res.value());
///   client.runIterate(100);  // caller drives the loop
class Client {
public:
    ~Client() noexcept;

    // Move-only
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ── Factory ──────────────────────────────────────────────────────────────

    /// Create a UA_Client, apply config, and connect to endpoint_url.
    /// Returns an error if UA_Client_connect does not return UA_STATUSCODE_GOOD.
    static sgrn::Result<Client> connect(ClientConfig t_cfg);

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Disconnect and free the underlying UA_Client. Idempotent.
    void disconnect() noexcept;

    bool isConnected() const noexcept {
        return connected_;
    }

    // ── Read / Write ─────────────────────────────────────────────────────────

    sgrn::Result<DataValue> readDataValue(const NodeId& t_node);
    sgrn::Result<void> writeDataValue(const NodeId& t_node, const DataValue& t_value);

    // ── Browse ───────────────────────────────────────────────────────────────

    /// Returns direct child NodeIds in the given direction.
    /// UA_BrowseResponse is always cleared before returning (success or error).
    sgrn::Result<std::vector<NodeId>> browse(const NodeId& t_start, UA_BrowseDirection t_direction = UA_BROWSEDIRECTION_FORWARD);

    /// Resolve a sequence of browse-name segments from start to a target NodeId.
    /// UA_TranslateBrowsePathsToNodeIdsResponse always cleared before returning.
    sgrn::Result<NodeId> translateBrowsePath(const NodeId& t_start, const std::vector<std::string>& t_path_elements);

    // ── Subscriptions ────────────────────────────────────────────────────────

    /// Returns the subscription id on success.
    /// Error if called while disconnected.
    sgrn::Result<UA_UInt32> createSubscription(uint32_t t_publish_interval_ms);

    /// Register a data-change monitored item.
    /// The callback receives a DataValueView — never a raw UA_* type.
    /// Error if called while disconnected.
    sgrn::Result<UA_UInt32> addMonitoredItem(
        UA_UInt32 t_subscription_id, const NodeId& t_node, std::function<void(const DataValueView&)> t_callback);

    // ── Event loop ───────────────────────────────────────────────────────────

    /// Drive the client event loop for one tick.
    /// When auto_reconnect is true and a disconnect is detected, sets an
    /// internal flag; on the next call (after reconnect_delay_ms has elapsed)
    /// attempts UA_Client_connect again. No background thread — the caller
    /// controls the loop.
    sgrn::Result<void> runIterate(uint32_t t_timeout_ms = 100);

    // ── INTERNAL: escape hatch for the adapter layer only ───────────────────
    UA_Client* raw() noexcept {
        return client_;
    }

private:
    explicit Client(UA_Client* tp_client, ClientConfig t_cfg);

    // Trampoline registered with open62541 for data-change notifications.
    // Wraps the raw UA_DataValue* in a DataValueView and dispatches to the
    // stored std::function — callers never see UA_* types.
    static void dataChangeCallback(
        UA_Client* tp_client, UA_UInt32 t_sub_id, void* tp_sub_context, UA_UInt32 t_mon_id, void* tp_mon_context, UA_DataValue* tp_value);

    UA_Client* client_{nullptr};
    ClientConfig cfg_;
    bool connected_{false};

    // Monitored-item dispatch table: monitored_item_id → callback
    std::unordered_map<UA_UInt32, std::function<void(const DataValueView&)>> mon_callbacks_;

    // Reconnect state (only meaningful when cfg_.auto_reconnect == true)
    bool needs_reconnect_{false};
    std::chrono::steady_clock::time_point reconnect_after_{};
};

} // namespace sgrn::gateway::wrappers::opcua
