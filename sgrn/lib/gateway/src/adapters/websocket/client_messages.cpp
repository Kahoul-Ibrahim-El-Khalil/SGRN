// WebSocketAdapter inbound client-message handling (clients -> adapter).
// Split from WebSocketAdapter.cpp; handles subscribe/unsubscribe/command dispatch.
#include <fmt/core.h>
#include <sgrn/common/endian_helper.hpp>
#include <sgrn/common/json_helper.hpp>
#include <sgrn/common/path_utils.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/RuntimeSyncProtocol.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/common/event_helper.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <string>

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
 * Connect to ws://<host>:<http-port>/ws — the WebSocket endpoint shares the
 * HTTP listener and port (single northbound server).
 *
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
void WebSocketAdapter::handleClientMessage(crow::websocket::connection& t_conn, const std::string& t_message) {
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
        if (cmd == "write" && doc.HasMember("updates") && doc["updates"].IsArray()) {
            // JSON is the inspectable equivalent of the binary RuntimeSync
            // frame. It is intentionally raw-byte based so typed clients can
            // use the schema they already negotiated instead of losing PLC
            // endianness or padding information in JSON conversion.
            bool ok = binary_write_fn_ != nullptr;
            std::string error;
            std::string ip;
            std::string origin;
            std::vector<std::string> headers;
            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto client = clients_.find(&t_conn);
                if (client != clients_.end()) {
                    ip = client->second.ip;
                    origin = client->second.origin;
                    headers = client->second.headers;
                } else {
                    ok = false;
                    error = "unknown WebSocket client";
                }
            }

            if (ok) {
                for (const auto& item : doc["updates"].GetArray()) {
                    if (!item.IsObject() || !item.HasMember("db") || !item["db"].IsUint() || item["db"].GetUint() > 65535 ||
                        !item.HasMember("offset") || !item["offset"].IsUint64() || !item.HasMember("size") || !item["size"].IsUint64() ||
                        !item.HasMember("data") || !item["data"].IsString()) {
                        ok = false;
                        error = "invalid RuntimeSync write record";
                        break;
                    }
                    const uint16_t db = static_cast<uint16_t>(item["db"].GetUint());
                    const uint64_t offset = item["offset"].GetUint64();
                    const uint64_t size = item["size"].GetUint64();
                    if (offset > std::numeric_limits<size_t>::max() || size == 0 || size > std::numeric_limits<size_t>::max()) {
                        ok = false;
                        error = "RuntimeSync write range is invalid";
                        break;
                    }
                    if (security_manager_) {
                        auto auth =
                            SecurityHelper::authorizeWrite(*security_manager_, security::Protocol::WebSocket, ip, db, "", origin, headers);
                        if (auth.hasError()) {
                            ok = false;
                            error = auth.error();
                            break;
                        }
                    }
                    auto payload = sgrn::utils::encoding::fromBase64(item["data"].GetString());
                    if (payload.size() < size) {
                        ok = false;
                        error = "RuntimeSync write payload is shorter than size";
                        break;
                    }
                    payload.resize(static_cast<size_t>(size));
                    if (auto result = binary_write_fn_(db, static_cast<size_t>(offset), payload.size(), payload.data());
                        result.hasError()) {
                        ok = false;
                        error = result.error();
                        break;
                    }
                }
            }

            rapidjson::StringBuffer ack_buffer;
            rapidjson::Writer<rapidjson::StringBuffer> ack_writer(ack_buffer);
            ack_writer.StartObject();
            ack_writer.Key("type");
            ack_writer.String("write_ack");
            if (doc.HasMember("sequence") && doc["sequence"].IsUint64()) {
                ack_writer.Key("sequence");
                ack_writer.Uint64(doc["sequence"].GetUint64());
            }
            ack_writer.Key("ok");
            ack_writer.Bool(ok);
            if (!ok && !error.empty()) {
                ack_writer.Key("error");
                ack_writer.String(error.c_str());
            }
            ack_writer.EndObject();
            sendText(t_conn, ack_buffer.GetString());
        } else if (cmd == "write_area" && doc.HasMember("updates") && doc["updates"].IsArray()) {
            // Discrete-area twin writes (TIA-style tags: PE/PA/MK arenas).
            // Same shape as "write" but addressed by S7 area code instead of
            // DB number: {"area":129,"offset":0,"size":1,"data":"base64url"}.
            // Unknown to older servers (silently ignored, no ack); servers
            // without an area hook NACK with a clear error.
            AreaWriteFn area_write_fn;
            {
                std::lock_guard<std::mutex> lk(area_write_mutex_);
                area_write_fn = area_write_fn_;
            }
            bool area_ok = area_write_fn != nullptr;
            std::string area_error = area_ok ? "" : "discrete-area writes not supported by this gateway";
            std::string ip;
            std::string origin;
            std::vector<std::string> headers;
            if (area_ok) {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto client = clients_.find(&t_conn);
                if (client != clients_.end()) {
                    ip = client->second.ip;
                    origin = client->second.origin;
                    headers = client->second.headers;
                } else {
                    area_ok = false;
                    area_error = "unknown WebSocket client";
                }
            }

            if (area_ok) {
                for (const auto& item : doc["updates"].GetArray()) {
                    if (!item.IsObject() || !item.HasMember("area") || !item["area"].IsUint() || item["area"].GetUint() > 65535 ||
                        !item.HasMember("offset") || !item["offset"].IsUint64() || !item.HasMember("size") || !item["size"].IsUint64() ||
                        !item.HasMember("data") || !item["data"].IsString()) {
                        area_ok = false;
                        area_error = "invalid RuntimeSync area write record";
                        break;
                    }
                    const uint16_t area = static_cast<uint16_t>(item["area"].GetUint());
                    const uint64_t offset = item["offset"].GetUint64();
                    const uint64_t size = item["size"].GetUint64();
                    if (offset > std::numeric_limits<size_t>::max() || size == 0 || size > std::numeric_limits<size_t>::max()) {
                        area_ok = false;
                        area_error = "RuntimeSync area write range is invalid";
                        break;
                    }
                    if (security_manager_) {
                        auto auth = SecurityHelper::authorizeWrite(
                            *security_manager_, security::Protocol::WebSocket, ip, std::nullopt, "", origin, headers);
                        if (auth.hasError()) {
                            area_ok = false;
                            area_error = auth.error();
                            break;
                        }
                    }
                    auto payload = sgrn::utils::encoding::fromBase64(item["data"].GetString());
                    if (payload.size() < size) {
                        area_ok = false;
                        area_error = "RuntimeSync area write payload is shorter than size";
                        break;
                    }
                    payload.resize(static_cast<size_t>(size));
                    if (auto result = area_write_fn(area, static_cast<size_t>(offset), payload.size(), payload.data()); result.hasError()) {
                        area_ok = false;
                        area_error = result.error();
                        break;
                    }
                }
            }

            rapidjson::StringBuffer area_ack_buffer;
            rapidjson::Writer<rapidjson::StringBuffer> area_ack_writer(area_ack_buffer);
            area_ack_writer.StartObject();
            area_ack_writer.Key("type");
            area_ack_writer.String("write_ack");
            if (doc.HasMember("sequence") && doc["sequence"].IsUint64()) {
                area_ack_writer.Key("sequence");
                area_ack_writer.Uint64(doc["sequence"].GetUint64());
            }
            area_ack_writer.Key("ok");
            area_ack_writer.Bool(area_ok);
            if (!area_ok && !area_error.empty()) {
                area_ack_writer.Key("error");
                area_ack_writer.String(area_error.c_str());
            }
            area_ack_writer.EndObject();
            sendText(t_conn, area_ack_buffer.GetString());
        } else if (cmd == "subscribe" && doc.HasMember("path") && doc["path"].IsString()) {
            std::string path = doc["path"].GetString();
            // Catch-up target: subscriptions resolved below; the current
            // values are pushed after the lock is released.
            crow::websocket::connection* catchup_conn = nullptr;
            std::vector<ClientContext::LeafRange> catchup_ranges;
            if (security_manager_ && registry_) {
                // Common schema_resolver — resolve path to schema info
                auto resolution = schema_resolver::resolve(path, *registry_);
                std::optional<uint16_t> db_num = resolution.schema ? std::optional<uint16_t>(resolution.schema->db_number) : std::nullopt;

                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(&t_conn);
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
                        catchup_conn = &t_conn;
                        catchup_ranges = it->second.leaf_ranges;
                    }
                }
            } else {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(&t_conn);
                if (it == clients_.end())
                    return;
                it->second.subscriptions.insert(path);
                resolveLeafRanges(it->second);
                if (it->second.dictionary_mode) {
                    catchup_conn = &t_conn;
                    catchup_ranges = it->second.leaf_ranges;
                }
            }
            if (catchup_conn)
                sendCatchUp(*catchup_conn, catchup_ranges);
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
                auto it = clients_.find(&t_conn);
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
            if (!sendBinaryFrame(&t_conn, *db_num, offset, size, timestamp_seconds)) {
                SGRN_WARN_LOG("WebSocket binary seed for DB{} offset {} size {} failed", *db_num, offset, size);
            }
        } else if (cmd == "unsubscribe" && doc.HasMember("path") && doc["path"].IsString()) {
            std::string path = doc["path"].GetString();
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(&t_conn);
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
            auto it = clients_.find(&t_conn);
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
            auto it = clients_.find(&t_conn);
            if (it != clients_.end()) {
                it->second.subscriptions.clear();
                it->second.binary_subscriptions.clear();
            }
        } else if (cmd == "setDictionaryMode") {
            const bool enabled = doc.HasMember("enabled") && doc["enabled"].IsBool() && doc["enabled"].GetBool();
            crow::websocket::connection* catchup_conn = nullptr;
            std::vector<ClientContext::LeafRange> catchup_ranges;
            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                auto it = clients_.find(&t_conn);
                if (it != clients_.end()) {
                    it->second.dictionary_mode = enabled;
                    resolveLeafRanges(it->second);
                    if (enabled) {
                        catchup_conn = &t_conn;
                        catchup_ranges = it->second.leaf_ranges;
                    }
                }
            }
            // A client that subscribed first (legacy) and opts into
            // dictionary mode afterwards gets current values right away.
            if (catchup_conn)
                sendCatchUp(*catchup_conn, catchup_ranges);
        }
    }
}

void WebSocketAdapter::handleBinaryMessage(crow::websocket::connection& t_conn, const std::string& t_message) {
    using namespace runtime_sync;

    Frame frame;
    std::string error;
    if (!decode(t_message, frame, &error) || frame.kind != Kind::Write) {
        SGRN_WARN_LOG("Rejected WebSocket RuntimeSync binary frame: {}", error.empty() ? "not a write frame" : error);
        return;
    }
    if (!binary_write_fn_ || frame.records.empty())
        return;

    std::string ip;
    std::string origin;
    std::vector<std::string> headers;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        auto client = clients_.find(&t_conn);
        if (client == clients_.end())
            return;
        ip = client->second.ip;
        origin = client->second.origin;
        headers = client->second.headers;
    }

    bool ok = true;
    for (const auto& record : frame.records) {
        if (record.bytes.empty()) {
            ok = false;
            error = "empty RuntimeSync write record";
            break;
        }
        if (security_manager_) {
            auto auth =
                SecurityHelper::authorizeWrite(*security_manager_, security::Protocol::WebSocket, ip, record.db, "", origin, headers);
            if (auth.hasError()) {
                ok = false;
                error = auth.error();
                break;
            }
        }
        if (auto result = binary_write_fn_(record.db, record.offset, record.bytes.size(), record.bytes.data()); result.hasError()) {
            ok = false;
            error = result.error();
            break;
        }
    }

    Frame ack;
    ack.kind = Kind::Ack;
    ack.sequence = frame.sequence;
    ack.timestamp_ms = frame.timestamp_ms;
    std::string encoded = encode(ack);
    if (ok && !encoded.empty()) {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        if (clients_.find(&t_conn) != clients_.end())
            sendBinary(t_conn, encoded.data(), encoded.size());
    } else {
        SGRN_WARN_LOG("RuntimeSync binary write failed: {}", error);
    }
}

} // namespace sgrn::gateway::adapters::websocket
