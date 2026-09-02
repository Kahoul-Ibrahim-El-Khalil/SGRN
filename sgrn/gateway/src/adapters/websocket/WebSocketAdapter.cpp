#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/common/endian_helper.hpp>
#include <sgrn/gateway/common/event_helper.hpp>
#include <sgrn/gateway/common/json_helper.hpp>
#include <sgrn/gateway/common/path_utils.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <ixwebsocket/IXWebSocket.h>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <limits>

using namespace sgrn::gateway::core;
using namespace sgrn::gateway::common;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::common::endian_helper::storeToBuffer;
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
    const PlcSchemaStore* tp_registry, std::function<std::string()> t_full_snapshot_provider, BinaryReadFn t_binary_read_fn) {
    security_manager_ = std::move(tsp_security_manager);
    registry_ = tp_registry;
    full_snapshot_provider_ = std::move(t_full_snapshot_provider);
    binary_read_fn_ = std::move(t_binary_read_fn);
    server_ = std::make_unique<ix::WebSocketServer>(t_port, t_ip);

    setupConnectionHandler();

    auto res = server_->listen();

    SGRN_RETURN_IF(!res.first, fmt::format("WebSocket server failed to listen: {}", res.second));

    server_->start();
    running_.store(true, std::memory_order_release);

    broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_event) { handleTelemetryEvent(t_event); });
    return {};
}

void WebSocketAdapter::setupConnectionHandler() {
    server_->setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
        auto tsp_ws = webSocket.lock();
        SGRN_RETURN_IF(!tsp_ws, ;);

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

                // Push the leaf dictionary once at connect time so clients
                // that opt into dictionary mode can decode flat id-keyed
                // payloads before the first batch arrives.
                if (dict_ && !dict_->id_to_path.empty()) {
                    rapidjson::StringBuffer dsb;
                    rapidjson::Writer<rapidjson::StringBuffer> dw(dsb);
                    dw.StartObject();
                    dw.Key("type");
                    dw.String("dictionary");
                    dw.Key("leaves");
                    dw.StartArray();
                    for (const auto& [id, path] : dict_->id_to_path) {
                        dw.StartObject();
                        dw.Key("id");
                        dw.Uint(id);
                        dw.Key("path");
                        dw.String(path.c_str(), static_cast<rapidjson::SizeType>(path.size()));
                        dw.EndObject();
                    }
                    dw.EndArray();
                    dw.EndObject();
                    ws_locked->send(dsb.GetString());
                }

                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[ws_locked] = ClientContext{connectionState->getRemoteIp(), origin, std::move(header_names), {}, {}, false, {}};
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
    auto binary_targets = collectBinaryTargets(t_event.db);

    if (targets.empty() && binary_targets.empty())
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
    std::unique_ptr<rapidjson::Document> parsed_doc;
    if (t_any_needs_filter) {
        parsed_doc = std::make_unique<rapidjson::Document>();
        parsed_doc->Parse(t_event.json_value->c_str());
        if (parsed_doc->HasParseError() || !parsed_doc->IsObject()) {
            // Parse failed — fall back to sending full JSON to all clients.
            parsed_doc.reset();
        }
    }

    // Send to each client — either full JSON (zero overhead) or filtered
    for (auto& target : targets) {
        if (target.dictionary_mode && dict_ && !dict_->path_to_id.empty()) {
            // Dictionary-mode client: flatten the nested delta snapshot into
            // a flat {"<leaf_id>": value} map, filtering by pre-resolved ranges.
            if (!parsed_doc) {
                parsed_doc = std::make_unique<rapidjson::Document>();
                parsed_doc->Parse(t_event.json_value->c_str());
                if (parsed_doc->HasParseError() || !parsed_doc->IsObject()) {
                    parsed_doc.reset();
                }
            }
            if (parsed_doc) {
                // Flatten the full snapshot once
                rapidjson::Document flat_doc;
                if (!twin::flattenNestedTree(*parsed_doc, dict_->path_to_id, flat_doc.GetAllocator(), flat_doc).hasError()) {
                    // If the client has specific subscriptions, filter by ranges
                    if (!target.leaf_ranges.empty()) {
                        rapidjson::Document filtered_doc;
                        filtered_doc.SetObject();
                        for (auto it = flat_doc.MemberBegin(); it != flat_doc.MemberEnd(); ++it) {
                            twin::LeafId id = static_cast<twin::LeafId>(std::stoul(it->name.GetString()));
                            for (const auto& range : target.leaf_ranges) {
                                if (id >= range.start && id <= range.end) {
                                    rapidjson::Value key;
                                    key.CopyFrom(it->name, filtered_doc.GetAllocator());
                                    rapidjson::Value val;
                                    val.CopyFrom(it->value, filtered_doc.GetAllocator());
                                    filtered_doc.AddMember(key, val, filtered_doc.GetAllocator());
                                    break;
                                }
                            }
                        }
                        rapidjson::StringBuffer filtered_sb;
                        rapidjson::Writer<rapidjson::StringBuffer> filtered_w(filtered_sb);
                        filtered_doc.Accept(filtered_w);
                        if (!filtered_doc.ObjectEmpty())
                            target.sp_ws->send(filtered_sb.GetString());
                    } else {
                        rapidjson::StringBuffer flat_sb;
                        rapidjson::Writer<rapidjson::StringBuffer> flat_w(flat_sb);
                        flat_doc.Accept(flat_w);
                        target.sp_ws->send(flat_sb.GetString());
                    }
                } else {
                    target.sp_ws->send(*t_event.json_value);
                }
            } else {
                target.sp_ws->send(*t_event.json_value);
            }
        } else if (target.needs_filter && parsed_doc) {
            // Non-dictionary filtered client: legacy path-based filtering
            std::set<std::string> dotted_subs;
            for (const auto& sub : target.field_subs) {
                dotted_subs.insert(path_utils::topicToPlcPath(sub));
            }
            auto payload = json_helper::filterFields(*parsed_doc, dotted_subs);
            if (!payload.empty() && payload != "{}")
                target.sp_ws->send(payload);
        } else {
            target.sp_ws->send(*t_event.json_value);
        }
    }

    if (binary_targets.empty())
        return;

    const double timestamp_seconds = static_cast<double>(t_event.timestamp) / 1000.0;
    for (auto& [key, sockets] : binary_targets) {
        const auto& [db, offset, size] = key;
        if (!registry_) {
            SGRN_WARN_LOG("WebSocket binary broadcast rejected: registry not available");
            continue;
        }
        auto schema_res = registry_->getDb(db);
        if (schema_res.hasError() || !schema_res.value()) {
            SGRN_WARN_LOG("WebSocket binary broadcast for DB{} offset {} size {} rejected: DB not found", db, offset, size);
            continue;
        }
        const auto* db_schema = schema_res.value();
        const size_t db_size = static_cast<size_t>(db_schema->size_bytes);
        if (offset > db_size || size > db_size - offset) {
            SGRN_WARN_LOG(
                "WebSocket binary broadcast for DB{} offset {} size {} rejected: range exceeds DB size {}", db, offset, size, db_size);
            continue;
        }

        std::vector<uint8_t> frame(12 + size);
        storeToBuffer<uint32_t>(static_cast<uint32_t>(db), frame.data(), s7codec::Endian::Big);
        storeToBuffer<double>(timestamp_seconds, frame.data() + 4, s7codec::Endian::Big);

        if (!binary_read_fn_) {
            SGRN_WARN_LOG("WebSocket binary read callback is not configured");
            continue;
        }

        auto read_res = binary_read_fn_(db, offset, size, frame.data() + 12);
        if (read_res.hasError()) {
            SGRN_WARN_LOG("WebSocket binary read for DB{} offset {} size {} failed: {}", db, offset, size, read_res.error());
            continue;
        }

        ix::IXWebSocketSendData payload(frame);
        for (const auto& sp_ws : sockets) {
            if (!sp_ws || sp_ws->getReadyState() != ix::ReadyState::Open)
                continue;
            auto send_info = sp_ws->sendBinary(payload);
            if (!send_info.success) {
                SGRN_WARN_LOG("WebSocket binary send to DB{} offset {} size {} failed", db, offset, size);
            }
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
                    targets.push_back(
                        {std::move(tsp_ws), true, it->second.dictionary_mode, std::move(t_field_subs), it->second.leaf_ranges});
                } else {
                    targets.push_back({std::move(tsp_ws), false, it->second.dictionary_mode, {}, it->second.leaf_ranges});
                }
            }
            ++it;
        } else {
            it = clients_.erase(it);
        }
    }

    return targets;
}

std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<std::shared_ptr<ix::WebSocket>>> WebSocketAdapter::collectBinaryTargets(
    uint16_t t_db) {
    std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<std::shared_ptr<ix::WebSocket>>> targets;

    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        auto tsp_ws = it->first;
        if (!tsp_ws || tsp_ws->getReadyState() != ix::ReadyState::Open) {
            it = clients_.erase(it);
            continue;
        }

        for (const auto& sub : it->second.binary_subscriptions) {
            if (sub.db != t_db)
                continue;
            targets[{sub.db, sub.offset, sub.size}].push_back(tsp_ws);
        }

        ++it;
    }

    return targets;
}

bool WebSocketAdapter::sendBinaryFrame(
    const std::shared_ptr<ix::WebSocket>& tsp_ws, uint16_t t_db, size_t t_offset, size_t t_size, double t_timestamp_seconds) {
    if (tsp_ws && tsp_ws->getReadyState() != ix::ReadyState::Open)
        return false;

    if (!registry_) {
        SGRN_WARN_LOG("WebSocket binary frame for DB{} offset {} size {} rejected: registry not available", t_db, t_offset, t_size);
        return false;
    }

    auto schema_res = registry_->getDb(t_db);
    if (schema_res.hasError() || !schema_res.value()) {
        SGRN_WARN_LOG("WebSocket binary frame for DB{} offset {} size {} rejected: DB not found", t_db, t_offset, t_size);
        return false;
    }

    const auto* db_schema = schema_res.value();
    const size_t db_size = static_cast<size_t>(db_schema->size_bytes);
    if (t_offset > db_size || t_size > db_size - t_offset) {
        SGRN_WARN_LOG(
            "WebSocket binary frame for DB{} offset {} size {} rejected: range exceeds DB size {}", t_db, t_offset, t_size, db_size);
        return false;
    }

    if (!binary_read_fn_) {
        SGRN_WARN_LOG("WebSocket binary frame for DB{} offset {} size {} rejected: read callback not configured", t_db, t_offset, t_size);
        return false;
    }

    std::vector<uint8_t> frame(12 + t_size);
    storeToBuffer<uint32_t>(static_cast<uint32_t>(t_db), frame.data(), s7codec::Endian::Big);
    storeToBuffer<double>(t_timestamp_seconds, frame.data() + 4, s7codec::Endian::Big);

    auto read_res = binary_read_fn_(t_db, t_offset, t_size, frame.data() + 12);
    if (read_res.hasError()) {
        SGRN_WARN_LOG("WebSocket binary read for DB{} offset {} size {} failed: {}", t_db, t_offset, t_size, read_res.error());
        return false;
    }

    if (!tsp_ws)
        return true;

    ix::IXWebSocketSendData payload(frame);
    auto send_info = tsp_ws->sendBinary(payload);
    if (!send_info.success) {
        SGRN_WARN_LOG("WebSocket binary send for DB{} offset {} size {} failed", t_db, t_offset, t_size);
        return false;
    }
    return true;
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

    auto resolveDbRef = [&](const rapidjson::Value& t_db_value) -> std::optional<uint16_t> {
        if (t_db_value.IsUint()) {
            return static_cast<uint16_t>(t_db_value.GetUint());
        }
        if (t_db_value.IsUint64() && t_db_value.GetUint64() <= std::numeric_limits<uint16_t>::max()) {
            return static_cast<uint16_t>(t_db_value.GetUint64());
        }
        if (t_db_value.IsString() && registry_) {
            return registry_->resolveDbRef(t_db_value.GetString());
        }
        return std::nullopt;
    };

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
                    resolveLeafRanges(it->second);
                }
            } else {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[tsp_ws].subscriptions.insert(path);
                resolveLeafRanges(clients_[tsp_ws]);
            }
        } else if (cmd == "subscribe_binary" && doc.HasMember("db")) {
            if (!registry_) {
                SGRN_WARN_LOG("WebSocket binary subscribe rejected: registry not available");
                return;
            }

            auto db_num = resolveDbRef(doc["db"]);
            if (!db_num.has_value()) {
                SGRN_WARN_LOG("WebSocket binary subscribe rejected: invalid DB reference");
                return;
            }

            size_t offset = 0;
            if (doc.HasMember("offset")) {
                if (!doc["offset"].IsUint64()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: invalid offset", *db_num);
                    return;
                }
                auto offset_raw = doc["offset"].GetUint64();
                if (offset_raw > std::numeric_limits<size_t>::max()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: offset out of range", *db_num);
                    return;
                }
                offset = static_cast<size_t>(offset_raw);
            }

            std::optional<size_t> size_opt;
            if (doc.HasMember("size")) {
                if (!doc["size"].IsUint64()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: invalid size", *db_num);
                    return;
                }
                auto size_raw = doc["size"].GetUint64();
                if (size_raw > std::numeric_limits<size_t>::max()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: size out of range", *db_num);
                    return;
                }
                size_opt = static_cast<size_t>(size_raw);
            }

            auto schema_res = registry_->getDb(*db_num);
            if (schema_res.hasError() || !schema_res.value()) {
                SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: DB not found", *db_num);
                return;
            }
            const auto* db_schema = schema_res.value();
            const size_t db_size = static_cast<size_t>(db_schema->size_bytes);
            size_t size = size_opt.value_or(db_size >= offset ? db_size - offset : 0);
            if (size == 0 || offset > db_size || size > db_size - offset) {
                SGRN_WARN_LOG(
                    "WebSocket binary subscribe from DB{} rejected: range {}+{} exceeds DB size {}", *db_num, offset, size, db_size);
                return;
            }

            std::string ip;
            std::string origin;
            std::vector<std::string> headers;
            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(tsp_ws);
                if (it == clients_.end())
                    return;

                ip = it->second.ip;
                origin = it->second.origin;
                headers = it->second.headers;

                const auto duplicate = std::find_if(it->second.binary_subscriptions.begin(), it->second.binary_subscriptions.end(),
                    [&](const ClientContext::BinarySubscription& sub) {
                        return sub.db == *db_num && sub.offset == offset && sub.size == size;
                    });
                if (duplicate != it->second.binary_subscriptions.end()) {
                    SGRN_WARN_LOG(
                        "WebSocket binary subscribe from {} to DB{} offset {} size {} ignored: duplicate", ip, *db_num, offset, size);
                    return;
                }

                if (security_manager_) {
                    auto auth =
                        SecurityHelper::authorizeRead(*security_manager_, security::Protocol::WebSocket, ip, db_num, "", origin, headers);
                    if (auth.hasError()) {
                        SGRN_WARN_LOG("WebSocket binary subscribe from {} to DB{} offset {} size {} denied", ip, *db_num, offset, size);
                        return;
                    }
                }

                it->second.binary_subscriptions.push_back(ClientContext::BinarySubscription{*db_num, offset, size});
            }

            const double timestamp_seconds = static_cast<double>(sgrn::utils::time::nowMilliseconds()) / 1000.0;
            if (!sendBinaryFrame(tsp_ws, *db_num, offset, size, timestamp_seconds)) {
                SGRN_WARN_LOG("WebSocket binary seed for DB{} offset {} size {} failed", *db_num, offset, size);
            }
        } else if (cmd == "unsubscribe" && doc.HasMember("path") && doc["path"].IsString()) {
            std::string path = doc["path"].GetString();
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it != clients_.end()) {
                it->second.subscriptions.erase(path);
                resolveLeafRanges(it->second);
            }
        } else if (cmd == "unsubscribe_binary" && doc.HasMember("db")) {
            if (!registry_) {
                SGRN_WARN_LOG("WebSocket binary unsubscribe rejected: registry not available");
                return;
            }

            auto db_num = resolveDbRef(doc["db"]);
            if (!db_num.has_value()) {
                SGRN_WARN_LOG("WebSocket binary unsubscribe rejected: invalid DB reference");
                return;
            }

            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it == clients_.end())
                return;

            const bool has_offset = doc.HasMember("offset");
            const bool has_size = doc.HasMember("size");
            if (!has_offset && !has_size) {
                auto& subs = it->second.binary_subscriptions;
                subs.erase(std::remove_if(
                               subs.begin(), subs.end(), [&](const ClientContext::BinarySubscription& sub) { return sub.db == *db_num; }),
                    subs.end());
            } else if (has_offset && has_size && doc["offset"].IsUint64() && doc["size"].IsUint64()) {
                auto offset_raw = doc["offset"].GetUint64();
                auto size_raw = doc["size"].GetUint64();
                if (offset_raw > std::numeric_limits<size_t>::max() || size_raw > std::numeric_limits<size_t>::max()) {
                    SGRN_WARN_LOG("WebSocket binary unsubscribe from DB{} rejected: range out of range", *db_num);
                    return;
                }
                size_t offset = static_cast<size_t>(offset_raw);
                size_t size = static_cast<size_t>(size_raw);
                auto& subs = it->second.binary_subscriptions;
                subs.erase(std::remove_if(subs.begin(), subs.end(),
                               [&](const ClientContext::BinarySubscription& sub) {
                                   return sub.db == *db_num && sub.offset == offset && sub.size == size;
                               }),
                    subs.end());
            } else {
                SGRN_WARN_LOG("WebSocket binary unsubscribe from DB{} rejected: offset/size must be provided together", *db_num);
                return;
            }
        } else if (cmd == "clear_subscriptions") {
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it != clients_.end()) {
                it->second.subscriptions.clear();
                it->second.binary_subscriptions.clear();
            }
        } else if (cmd == "setDictionaryMode") {
            const bool enabled = doc.HasMember("enabled") && doc["enabled"].IsBool() && doc["enabled"].GetBool();
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(tsp_ws);
            if (it != clients_.end()) {
                it->second.dictionary_mode = enabled;
                resolveLeafRanges(it->second);
            }
        }
    }
}

void WebSocketAdapter::stop() {
    if (broker_sub_id_ != 0) {
        TelemetryBroker::instance().unsubscribe(broker_sub_id_);
        broker_sub_id_ = 0;
    }
    if (server_) {
        server_->stop();
    }
    running_.store(false, std::memory_order_release);
}

void WebSocketAdapter::resolveLeafRanges(ClientContext& t_ctx) {
    t_ctx.leaf_ranges.clear();
    if (!dict_ || dict_->path_to_id.empty() || !t_ctx.dictionary_mode)
        return;

    // For each subscription, find all matching leaf ids and collect their ranges.
    // LeafDictionary assigns ids contiguously per DB in DB-ascending order,
    // so a whole-DB subscription is exactly one contiguous range.
    for (const auto& sub : t_ctx.subscriptions) {
        // Convert slash-separated topic path to dotted PLC path
        std::string dotted = path_utils::topicToPlcPath(sub);
        auto dot_pos = dotted.find('.');
        std::string sub_db = dot_pos != std::string::npos ? dotted.substr(0, dot_pos) : dotted;
        std::string sub_field = dot_pos != std::string::npos ? dotted.substr(dot_pos + 1) : "";

        for (const auto& [path, id] : dict_->path_to_id) {
            // Check if this leaf matches the subscription
            auto leaf_dot = path.find('.');
            std::string leaf_db = leaf_dot != std::string::npos ? path.substr(0, leaf_dot) : path;
            std::string leaf_field = leaf_dot != std::string::npos ? path.substr(leaf_dot + 1) : "";

            if (sub_db != leaf_db)
                continue;

            // DB-level subscription: all fields in the DB match
            if (sub_field.empty()) {
                t_ctx.leaf_ranges.push_back({id, id});
                continue;
            }

            // Field-level subscription: exact match or prefix match
            if (leaf_field == sub_field || (leaf_field.substr(0, sub_field.size()) == sub_field &&
                                               (leaf_field.size() == sub_field.size() || leaf_field[sub_field.size()] == '.'))) {
                t_ctx.leaf_ranges.push_back({id, id});
            }
        }
    }

    // Merge overlapping/adjacent ranges for efficiency
    if (t_ctx.leaf_ranges.size() > 1) {
        std::sort(t_ctx.leaf_ranges.begin(), t_ctx.leaf_ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        std::vector<ClientContext::LeafRange> merged;
        merged.push_back(t_ctx.leaf_ranges[0]);
        for (size_t i = 1; i < t_ctx.leaf_ranges.size(); ++i) {
            auto& back = merged.back();
            if (t_ctx.leaf_ranges[i].start <= back.end + 1) {
                back.end = std::max(back.end, t_ctx.leaf_ranges[i].end);
            } else {
                merged.push_back(t_ctx.leaf_ranges[i]);
            }
        }
        t_ctx.leaf_ranges = std::move(merged);
    }
}

} // namespace sgrn::gateway::adapters::websocket
