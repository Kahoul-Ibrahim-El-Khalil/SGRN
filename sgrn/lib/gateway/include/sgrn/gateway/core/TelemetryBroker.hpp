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
#include <rapidjson/stringbuffer.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

    // DB numbers covered by a multi-DB DeltaSnapshot. Single-DB events
    // (LeafUpdate, per-DB snapshots) use `db` above; a coalesced
    // DeltaSnapshot leaves `db` at 0 and lists every dirty DB here so
    // per-DB consumers (binary WAL writer) can snapshot each image.
    // Empty for single-DB events.
    std::vector<uint16_t> dirty_dbs;

    // When true, json_value is already a flat numeric-keyed JSON object
    // {"<leaf_id>": value, ...} — WebSocket adapter must skip flattenNestedTree.
    bool is_flat = false;

    bool is_valid() const {
        return json_value != nullptr || (typed_leaf.bytes && typed_leaf.meta.valid);
    }

    /**
     * @brief Lazily-parsed shared DOM of json_value.
     *
     * Parsed at most once per event, on first use, and shared by every
     * subscriber that inspects the payload (WebSocket filtering,
     * persistence, ...). Paths that never inspect the DOM — e.g. the
     * WebSocket firehose, which forwards the string untouched — pay nothing.
     * All broker callbacks for one event run sequentially on the broker
     * strand, so no locking is needed around the mutable cache.
     *
     * @return Pointer to the parsed object DOM, or nullptr when the event
     * carries no usable JSON object. Callers fall back to their unfiltered
     * behavior on null, exactly as on a local parse failure.
     */
    const rapidjson::Document* parsedJson() const {
        if (!parsed_attempted_) {
            parsed_attempted_ = true;
            if (json_value && !json_value->empty()) {
                auto doc = std::make_shared<rapidjson::Document>();
                doc->Parse(json_value->c_str());
                if (!doc->HasParseError() && doc->IsObject())
                    parsed_cache_ = std::move(doc);
            }
        }
        return parsed_cache_.get();
    }

    // Implementation detail for parsedJson() — do not touch directly.
    // (Kept public: TelemetryEvent is constructed with designated
    // initializers at publish sites, which requires an aggregate.)
    mutable bool parsed_attempted_ = false;
    mutable std::shared_ptr<rapidjson::Document> parsed_cache_;
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
 *      - WebSocketAdapter: sends directly (zero-copy) or filters fields (shared DOM + re-serialize)
 *      - PersistenceService: shared DOM, applies namespace filter, re-serializes fields, compresses
 *      - DatastoreBridge: consumes WAL files, not telemetry (unaffected)
 *      - OPC-UA adapter: typed_leaf payloads, not JSON (unaffected)
 *
 * The parsed DOM is built LAZILY and at most once per event
 * (TelemetryEvent::parsedJson(), shared by all subscribers). Paths that
 * never inspect the payload — e.g. the WebSocket firehose — pay nothing.
 *
 * This keeps the loose-coupling PRO (subscribers stay independent) while
 * removing the old CON (one full JSON parse per inspecting subscriber).
 *
 * PERFORMANCE IMPLICATIONS:
 *   - WebSocket firehose: ~0μs overhead (optimal)
 *   - WebSocket field-filtered: one shared parse + filter + re-serialize
 *   - Persistence: shared parse + filter + re-serialize + compress
 *   - The compression step dominates; parsing overhead is ~25-30% of total cost.
 *
 * If you need to optimize, consider:
 *   - Increasing atomic_window_ms to reduce parse/compress frequency
 *   - Using namespaces filter aggressively to reduce persistence work
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

        // By posting the batch to a single strand_ instead of the raw io_context, we guarantee that:
        // 1. All events are dequeued and processed in the EXACT order they were published.
        // 2. We preserve strict determinism even if the underlying io_context runs on a thread pool.
        // 3. We avoid thread-contention (no mutex needed in subscribers like PersistenceService).
        // 4. The fast, non-blocking subscribers (like WebSocket queues and Zstd memory buffers)
        //    execute sequentially without blocking the S7 polling thread that calls publish().
        asio::post(strand_, [snapshot = std::move(snapshot), shared_event]() {
            for (const auto& t_cb : snapshot)
                t_cb(*shared_event);
        });
    }

    /// Thread-local RapidJSON buffer stream to eliminate malloc/free churn on telemetry ticks
    static rapidjson::StringBuffer& threadLocalStringBuffer() {
        thread_local rapidjson::StringBuffer sb;
        sb.Clear();
        return sb;
    }

private:
    TelemetryBroker()
        : strand_(asio::make_strand(sgrn::gateway::core::GlobalContext::instance().io_context())) {
    }
    std::unordered_map<SubscriberId, Callback> subs_;
    std::shared_mutex mutex_;
    std::atomic<SubscriberId> next_id_{0};
    asio::strand<asio::io_context::executor_type> strand_;
};

} // namespace sgrn::gateway::core
