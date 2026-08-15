#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/common/event_helper.hpp>
#include <sgrn/gateway/common/json_helper.hpp>
#include <sgrn/gateway/common/path_utils.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace sgrn::gateway::core;
using namespace sgrn::gateway::common;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::core::EventType;
using sgrn::gateway::core::TelemetryBroker;
using sgrn::gateway::core::TelemetryEvent;
using sgrn::scl::PlcSchemaStore;
namespace sgrn::gateway::adapters::websocket
{

WebSocketAdapter::WebSocketAdapter() = default;
WebSocketAdapter::~WebSocketAdapter() {
    stop();
}

sgrn::Result<void> WebSocketAdapter::start(const std::string& t_ip, uint16_t t_port, SecurityManagerSptr tsp_security_manager,
    const PlcSchemaStore* tp_registry, std::function<std::string()> t_full_snapshot_provider) {
    security_manager_ = std::move(tsp_security_manager);
    registry_ = tp_registry;
    full_snapshot_provider_ = std::move(t_full_snapshot_provider);
    server_ = std::make_unique<ix::WebSocketServer>(t_port, t_ip);

    setupConnectionHandler();

    auto res = server_->listen();
    if (!res.first) {
        return fmt::format("WebSocket server failed to listen: {}", res.second);
    }
    server_->start();
    running_.store(true, std::memory_order_release);

    broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_event) { handleTelemetryEvent(t_event); });
    return {};
}

void WebSocketAdapter::setupConnectionHandler() {
    server_->setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
        auto tsp_ws = webSocket.lock();
        if (!tsp_ws)
            return;

        // The client is NOT registered in clients_ here. It is only added once
        // the Open handshake has completed and we have queued the current full
        // plant snapshot, guaranteeing that the delta stream (TelemetryBroker)
        // never reaches the client before its initial state frame.

        tsp_ws->setOnMessageCallback([this, webSocket, connectionState](const ix::WebSocketMessagePtr& msg) {
            auto ws_locked = webSocket.lock();
            if (!ws_locked)
                return;

            if (msg->type == ix::WebSocketMessageType::Open) {
                std::vector<std::string> header_names;
                std::string origin = "";
                for (const auto& [k, v] : msg->openInfo.headers) {
                    header_names.push_back(k);
                    if (k == "Origin" || k == "origin") {
                        origin = v;
                    }
                }
                if (security_manager_) {
                    // Common SecurityHelper for connection authorization
                    auto auth = SecurityHelper::authorizeConnection(
                        *security_manager_, security::Protocol::WebSocket, connectionState->getRemoteIp(), std::nullopt, origin);
                    if (auth.hasError()) {
                        SGRN_WARN_LOG("WebSocket connection from {} denied", connectionState->getRemoteIp());
                        ws_locked->close();
                        return;
                    }
                }

                // Seed the freshly-connected client with the current full plant
                // state before any DeltaSnapshot can be forwarded. After a
                // gateway restart this is the persisted ("reclaimed") twin, so
                // a viewer never sees an empty process image until the PLC
                // happens to push a delta. The client is only registered for
                // delta broadcasts AFTER this frame is queued, which pins the
                // wire ordering to: full snapshot → deltas.
                if (full_snapshot_provider_) {
                    std::string initial = full_snapshot_provider_();
                    if (!initial.empty() && initial != "{}") {
                        ws_locked->send(initial);
                    }
                }

                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[ws_locked] = ClientContext{connectionState->getRemoteIp(), origin, std::move(header_names), {}};
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_.erase(ws_locked);
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                handleClientMessage(ws_locked, msg->str);
            }
        });
    });
}

void WebSocketAdapter::handleTelemetryEvent(const TelemetryEvent& t_event) {
    if (t_event.type != EventType::DeltaSnapshot)
        return;

    // Collect target clients and determine if any needs field-level filtering
    bool t_any_needs_filter = false;
    auto targets = collectTargets(t_event, t_any_needs_filter);

    if (targets.empty())
        return;

    // ── PERFORMANCE NOTE: Shared JSON Parsing ─────────────────────────────────
    // The TelemetryBroker broadcasts the SAME shared_ptr<string> to all subscribers
    // (WebSocket, Persistence, DatastoreBridge, OPC-UA). Each subscriber may parse it
    // independently. This is an intentional architectural trade-off for loose coupling.
    //
    // WebSocket optimization paths:
    //   - Firehose mode (no subscriptions): sends JSON directly, zero-copy, ~0μs overhead
    //   - Field-filtered mode: parses JSON once, reuses parsed document for all filtered clients
    //
    // If ANY client needs field-level filtering, we parse the JSON once here.
    // The parsed document is reused for all filtered clients to avoid duplicate parsing.
    // This is still additional work beyond firehose mode, but avoids N parses for N clients.
    // ──────────────────────────────────────────────────────────────────────────

    // Parse the full JSON once if any client needs field-level filtering.
    // The parsed document is reused for all filtered clients.
    std::unique_ptr<rapidjson::Document> t_parsed_doc;
    if (t_any_needs_filter) {
        t_parsed_doc = std::make_unique<rapidjson::Document>();
        t_parsed_doc->Parse(t_event.json_value->c_str());
        if (t_parsed_doc->HasParseError() || !t_parsed_doc->IsObject()) {
            // Parse failed — fall back to sending full JSON to all
            for (auto& target : targets)
                target.sp_ws->send(*t_event.json_value);
            return;
        }
    }

    // Send to each client — either full JSON (zero overhead) or filtered
    for (auto& target : targets) {
        if (target.needs_filter && t_parsed_doc) {
            // Convert slash-separated subscription paths to dotted PLC paths
            // for the common json_helper::filterFields utility.
            std::set<std::string> dotted_subs;
            for (const auto& sub : target.field_subs) {
                dotted_subs.insert(path_utils::topicToPlcPath(sub));
            }
            auto payload = json_helper::filterFields(*t_parsed_doc, dotted_subs);
            target.sp_ws->send(payload);
        } else {
            target.sp_ws->send(*t_event.json_value);
        }
    }
}

std::vector<WebSocketAdapter::TargetInfo> WebSocketAdapter::collectTargets(const TelemetryEvent& t_event, bool& t_any_needs_filter) {

    std::vector<TargetInfo> targets;
    t_any_needs_filter = false;

    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        auto tsp_ws = it->first;
        if (tsp_ws && tsp_ws->getReadyState() == ix::ReadyState::Open) {
            const auto& subs = it->second.subscriptions;

            // Common event_filter::shouldSend — firehose mode or path overlap
            bool send = event_filter::shouldSend(t_event.dirty_paths, subs);

            if (send) {
                // Common event_filter::needsFieldFiltering — DB-level wins over field-level
                bool needs_filter = event_filter::needsFieldFiltering(t_event.dirty_paths, subs);
                std::set<std::string> t_field_subs;

                if (needs_filter) {
                    // Collect the field-level subscriptions that overlap dirty paths
                    for (const auto& sub : subs) {
                        if (sub.find('/') == std::string::npos)
                            continue; // DB-level subscription — not field-level
                        std::string sub_dotted = path_utils::topicToPlcPath(sub);
                        for (const auto& dirty : t_event.dirty_paths) {
                            std::string dirty_str = dirty.toDotted();
                            // MED-6: Require a dot boundary so "React" does not
                            // accidentally match "ReactorCore".
                            const bool exact_or_sub = dirty_str == sub_dotted || dirty_str.starts_with(sub_dotted + ".") ||
                                                      sub_dotted.starts_with(dirty_str + ".");
                            if (exact_or_sub) {
                                t_field_subs.insert(sub);
                                break;
                            }
                        }
                    }
                }

                if (needs_filter) {
                    t_any_needs_filter = true;
                    targets.push_back({std::move(tsp_ws), true, std::move(t_field_subs)});
                } else {
                    targets.push_back({std::move(tsp_ws), false, {}});
                }
            }
            ++it;
        } else {
            it = clients_.erase(it);
        }
    }

    return targets;
}

/**
 * @brief Handles incoming WebSocket messages from clients.
 *
 * WebSocket API Documentation
 * ───────────────────────────
 * Clients can dynamically subscribe to specific PLC paths to filter the
 * telemetry stream. By default, clients receive nothing (or everything,
 * depending on integration). To control the stream, send JSON commands:
 *
 * 1. Subscribe to a path
 *    Payload: {"command": "subscribe", "path": "ReactorCore/speed"}
 *    Behavior: The gateway will now push delta updates for this path
 *              whenever it changes in the PLC.
 *
 * 2. Unsubscribe from a path
 *    Payload: {"command": "unsubscribe", "path": "ReactorCore/speed"}
 *    Behavior: The gateway stops pushing updates for this path.
 *
 * Downstream Telemetry Format
 * ───────────────────────────
 * When a subscribed path changes, the gateway pushes a JSON DeltaSnapshot.
 * The payload is ALWAYS rooted at the top-level DataBlock name:
 *
 *    { "ReactorCore": { "thermal_power_mw": 100.5, "speed": 12.0 } }
 *
 * Note: When a client subscribes to a specific field path (e.g.,
 * "ReactorCore/speed"), the gateway prunes the JSON server-side to
 * only include the requested fields. Clients subscribing to an entire
 * Data Block (e.g., "ReactorCore") still receive the full DB object.
 * This is transparent to the frontend — the JSON structure is identical,
 * just pruned.
 */
void WebSocketAdapter::handleClientMessage(std::shared_ptr<ix::WebSocket> tsp_ws, const std::string& t_message) {
    // MED-5: Reject oversized messages before parsing to prevent memory exhaustion.
    constexpr size_t kMaxMessageBytes = 4096;
    if (t_message.size() > kMaxMessageBytes)
        return;

    rapidjson::Document doc;
    doc.Parse(t_message.c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return;

    if (doc.HasMember("command") && doc["command"].IsString()) {
        std::string cmd = doc["command"].GetString();
        if (cmd == "subscribe" && doc.HasMember("path") && doc["path"].IsString()) {
            std::string path = doc["path"].GetString();
            if (security_manager_ && registry_) {
                // Common schema_resolver — resolve path to schema info
                auto resolution = schema_resolver::resolve(path, *registry_);
                std::optional<uint16_t> db_num = resolution.schema ? std::optional<uint16_t>(resolution.schema->db_number) : std::nullopt;

                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(tsp_ws);
                if (it != clients_.end()) {
                    // Common SecurityHelper for field-level read authorization
                    auto auth = SecurityHelper::authorizeRead(*security_manager_, security::Protocol::WebSocket, it->second.ip, db_num,
                        resolution.field_path, it->second.origin, it->second.headers);
                    if (auth.hasError()) {
                        SGRN_WARN_LOG("WebSocket subscribe from {} to path {} denied", it->second.ip, path);
                        return;
                    }
                    it->second.subscriptions.insert(path);
                }
            } else {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[tsp_ws].subscriptions.insert(path);
            }
        } else if (cmd == "unsubscribe" && doc.HasMember("path") && doc["path"].IsString()) {
            std::string path = doc["path"].GetString();
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it != clients_.end()) {
                it->second.subscriptions.erase(path);
            }
        } else if (cmd == "clear_subscriptions") {
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it != clients_.end()) {
                it->second.subscriptions.clear();
            }
        }
    }
}

void WebSocketAdapter::stop() {
    if (broker_sub_id_ != 0) {
        TelemetryBroker::instance().unsubscribe(broker_sub_id_);
        broker_sub_id_ = 0;
    }
    if (server_)
        server_->stop();
    running_.store(false, std::memory_order_release);
}

} // namespace sgrn::gateway::adapters::websocket
