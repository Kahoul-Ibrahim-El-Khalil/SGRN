#pragma once

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/core/LeafFieldMeta.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <rapidjson/document.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace sgrn::gateway::core
{

enum class EventType {
    LeafUpdate,    // Single tag update (for UI/REST)
    DeltaSnapshot, // Partial tree (for Cloud deltas)
    FullSnapshot   // Full plant image (for Cloud anchors)
};

/**
 * @brief Represents a single telemetry update event.
 */
struct TelemetryEvent {
    EventType type = EventType::LeafUpdate;
    uint16_t db = 0;
    std::string path;
    std::shared_ptr<std::string> json_value;
    TypedLeafPayload typed_leaf;
    uint64_t timestamp = 0;

    // Tier 1: Include specific dirty paths
    std::vector<sgrn::gateway::twin::TreePath> dirty_paths;

    bool is_valid() const {
        return json_value != nullptr || (typed_leaf.bytes && typed_leaf.meta.valid);
    }
};

/**
 * @brief High-Performance Telemetry Broker for Gateway.
 *
 * ARCHITECTURAL NOTE: Shared Event Model
 * ──────────────────────────────────────
 * All northbound adapters (WebSocket, Persistence, DatastoreBridge, OPC-UA)
 * subscribe to the SAME TelemetryEvent stream. When a dirty path changes:
 *
 *   1. PlcState::getDeltaSnapshot() serializes the delta ONCE to JSON
 *   2. TelemetryBroker::publish() broadcasts a shared_ptr<string> to all subscribers
 *   3. Each subscriber processes the JSON independently:
 *      - WebSocketAdapter: sends directly (zero-copy) or filters fields (parse + re-serialize)
 *      - PersistenceService: parses JSON, applies namespace filter, re-serializes fields, compresses
 *      - DatastoreBridge: parses JSON, batches, compresses
 *      - OPC-UA adapter: parses JSON, converts to OPC-UA types
 *
 * This means the JSON may be parsed MULTIPLE TIMES per event (once per subscriber
 * that needs to inspect the content). This is an intentional architectural trade-off:
 *
 *   PRO: Loose coupling - subscribers are independent, can be added/removed without
 *        coordinating with each other. Each adapter runs on its own thread pool.
 *
 *   CON: Duplicate parsing work when multiple subscribers need to inspect the JSON.
 *        WebSocket (firehose mode) avoids this by sending the string directly.
 *
 * PERFORMANCE IMPLICATIONS:
 *   - WebSocket firehose: ~0μs overhead (optimal)
 *   - WebSocket field-filtered: ~200μs (parse + filter + re-serialize)
 *   - Persistence: ~700μs (parse + filter + re-serialize + compress)
 *   - The compression step dominates; parsing overhead is ~25-30% of total cost.
 *
 * If you need to optimize, consider:
 *   - Increasing atomic_window_ms to reduce parse/compress frequency
 *   - Using namespaces filter aggressively to reduce persistence work
 *   - Accepting that this is a reasonable trade-off for architectural cleanliness
 */
class TelemetryBroker {
public:
    using SubscriberId = uint32_t;
    using Callback = std::function<void(const TelemetryEvent&)>;

    static TelemetryBroker& instance() {
        static TelemetryBroker inst;
        return inst;
    }

    SubscriberId subscribe(Callback t_cb) {
        std::unique_lock lock(mutex_);
        auto t_id = ++next_id_;
        subs_[t_id] = std::move(t_cb);
        return t_id;
    }

    void unsubscribe(SubscriberId t_id) {
        std::unique_lock lock(mutex_);
        subs_.erase(t_id);
    }

    void publish(TelemetryEvent t_event) {
        auto shared_event = std::make_shared<TelemetryEvent>(std::move(t_event));
        // Snapshot callbacks under shared_lock, then post a single task
        std::vector<Callback> snapshot;
        {
            std::shared_lock lock(mutex_);
            snapshot.reserve(subs_.size());
            for (const auto& [t_id, t_cb] : subs_)
                snapshot.push_back(t_cb);
        }
        asio::post(sgrn::gateway::core::GlobalContext::instance().io_context(), [snapshot = std::move(snapshot), shared_event]() {
            for (const auto& t_cb : snapshot)
                t_cb(*shared_event);
        });
    }

private:
    TelemetryBroker() = default;
    std::unordered_map<SubscriberId, Callback> subs_;
    std::shared_mutex mutex_;
    std::atomic<SubscriberId> next_id_{0};
};

} // namespace sgrn::gateway::core
