// WebSocketAdapter inbound client-message handling (clients -> adapter).
// Split from WebSocketAdapter.cpp; handles subscribe/unsubscribe/command dispatch.
#include <fmt/core.h>
#include <sgrn/common/endian_helper.hpp>
#include <sgrn/common/json_helper.hpp>
#include <sgrn/common/path_utils.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/common/event_helper.hpp>
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
using namespace sgrn::common;
using sgrn::common::endian_helper::storeToBuffer;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::core::EventType;
using sgrn::gateway::core::TelemetryBroker;
using sgrn::gateway::core::TelemetryEvent;
using sgrn::scl::PlcSchemaStore;
namespace sgrn::gateway::adapters::websocket
{

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
            // Catch-up target: subscriptions resolved below; the current
            // values are pushed after the lock is released.
            std::shared_ptr<ix::WebSocket> catchup_ws;
            std::vector<ClientContext::LeafRange> catchup_ranges;
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
                    if (it->second.dictionary_mode) {
                        catchup_ws = tsp_ws;
                        catchup_ranges = it->second.leaf_ranges;
                    }
                }
            } else {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[tsp_ws].subscriptions.insert(path);
                resolveLeafRanges(clients_[tsp_ws]);
                if (clients_[tsp_ws].dictionary_mode) {
                    catchup_ws = tsp_ws;
                    catchup_ranges = clients_[tsp_ws].leaf_ranges;
                }
            }
            sendCatchUp(catchup_ws, catchup_ranges);
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
            std::shared_ptr<ix::WebSocket> catchup_ws;
            std::vector<ClientContext::LeafRange> catchup_ranges;
            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(tsp_ws);
                if (it != clients_.end()) {
                    it->second.dictionary_mode = enabled;
                    resolveLeafRanges(it->second);
                    if (enabled) {
                        catchup_ws = tsp_ws;
                        catchup_ranges = it->second.leaf_ranges;
                    }
                }
            }
            // A client that subscribed first (legacy) and opts into
            // dictionary mode afterwards gets current values right away.
            sendCatchUp(catchup_ws, catchup_ranges);
        }
    }
}

} // namespace sgrn::gateway::adapters::websocket
