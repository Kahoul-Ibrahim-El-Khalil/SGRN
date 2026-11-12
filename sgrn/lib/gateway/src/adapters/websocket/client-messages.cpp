#include <sgrn/utils/json-dom.hpp>
// WebSocketAdapter inbound client-message handling (clients -> adapter).
// Split from WebSocketAdapter.cpp; handles subscribe/unsubscribe/command dispatch.
#include <fmt/core.h>
#include <sgrn/common/endian-helper.hpp>
#include <sgrn/common/event-helper.hpp>
#include <sgrn/common/path-utils.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/RuntimeSyncProtocol.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

namespace
{

namespace runtime_sync_json = ::sgrn::gateway::adapters::websocket::runtime_sync::json;

// Shared validation/decode/write mechanics for DB and discrete-area records.
// Authorization and the final memory write remain caller-provided because DB
// writes authorize a concrete DB number while area writes authorize no DB.
template <class UpdateRange, class RecordParser, class AddressGetter, class Authorize, class Write>
std::optional<std::string> applyRuntimeSyncWrites(const UpdateRange& t_updates, RecordParser&& t_parse_record,
    std::string_view t_invalid_record, std::string_view t_invalid_range, std::string_view t_short_payload, AddressGetter&& t_get_address,
    Authorize&& t_authorize, Write&& t_write) {
    for (const auto& raw_update : t_updates) {
        // Convert one record at a time so validation and writes retain the
        // original sequential/partial-application behavior without a second
        // JSON parse or an intermediate batch allocation.
        auto parsed_update = t_parse_record(raw_update);
        if (!parsed_update)
            return std::string(t_invalid_record);
        const auto& update = *parsed_update;
        const auto address = t_get_address(update);
        if (!address || !update.offset || !update.size || !update.data || *address > std::numeric_limits<uint16_t>::max())
            return std::string(t_invalid_record);

        const uint64_t offset = *update.offset;
        const uint64_t size = *update.size;
        if (offset > std::numeric_limits<size_t>::max() || size == 0 || size > std::numeric_limits<size_t>::max())
            return std::string(t_invalid_range);

        const uint16_t target = static_cast<uint16_t>(*address);
        if (auto error = t_authorize(target); error.has_value())
            return *error;

        auto payload = sgrn::utils::encoding::fromBase64(*update.data);
        if (payload.size() < size)
            return std::string(t_short_payload);
        payload.resize(static_cast<size_t>(size));

        if (auto result = t_write(target, static_cast<size_t>(offset), payload.size(), payload.data()); result.hasError())
            return result.error();
    }
    return std::nullopt;
}

std::string serializeWriteAck(const std::optional<uint64_t>& t_sequence, bool t_ok, const std::string& t_error) {
    runtime_sync_json::WriteAck ack;
    ack.type = "write_ack";
    ack.sequence = t_sequence;
    ack.ok = t_ok;
    if (!t_ok && !t_error.empty())
        ack.error = t_error;

    auto serialized = sgrn::json::writeOmitNull(ack);
    if (!serialized.hasError())
        return serialized.value();

    SGRN_WARN_LOG("Could not serialize RuntimeSync write acknowledgement: {}", serialized.error().message);
    return R"({"type":"write_ack","ok":false,"error":"ack serialization failed"})";
}

} // namespace

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
    RETURN_IF(t_message.size() > kMaxMessageBytes, ;);
    sgrn::json::dom::Document doc;
    doc.parse(t_message.c_str());
    RETURN_IF(doc.hasParseError() || !doc.is_object(), ;);

    auto resolveDbRef = [&](const sgrn::json::dom::Value& t_db_value) -> std::optional<uint16_t> {
        RETURN_IF(t_db_value.isUint(), static_cast<uint16_t>(t_db_value.getUint()));

        RETURN_IF(t_db_value.isUint64() && t_db_value.getUint64() <= std::numeric_limits<uint16_t>::max(),
            static_cast<uint16_t>(t_db_value.getUint64()));

        RETURN_IF(t_db_value.is_string() && registry_, registry_->resolveDbRef(t_db_value.getString()));

        return std::nullopt;
    };

    if (doc.hasMember("command") && doc["command"].is_string()) {
        std::string cmd = doc["command"].getString();
        if (cmd == "write" && doc.hasMember("updates") && doc["updates"].is_array()) {
            // JSON writes share their reflected DTO with GatewaySync. Reuse the
            // already-parsed Glaze JSON DOM records and adapt them one at a time, so
            // there is no second parse or intermediate batch allocation.
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

            std::optional<uint64_t> sequence;
            if (doc.hasMember("sequence") && doc["sequence"].isUint64())
                sequence = doc["sequence"].getUint64();

            if (ok) {
                if (auto write_error = applyRuntimeSyncWrites(
                        doc["updates"].elements(),
                        [](const sgrn::json::dom::Value& item) -> std::optional<runtime_sync_json::DbWriteRecord> {
                            if (!item.is_object() || !item.hasMember("db") || !item["db"].isUint() || item["db"].getUint() > 65535 ||
                                !item.hasMember("offset") || !item["offset"].isUint64() || !item.hasMember("size") ||
                                !item["size"].isUint64() || !item.hasMember("data") || !item["data"].is_string())
                                return std::nullopt;
                            runtime_sync_json::DbWriteRecord record;
                            record.db = item["db"].getUint();
                            record.offset = item["offset"].getUint64();
                            record.size = item["size"].getUint64();
                            record.data = item["data"].getString();
                            return record;
                        },
                        "invalid RuntimeSync write record", "RuntimeSync write range is invalid",
                        "RuntimeSync write payload is shorter than size",
                        [](const runtime_sync_json::DbWriteRecord& item) { return item.db; },
                        [&](uint16_t db) -> std::optional<std::string> {
                            if (!security_manager_)
                                return std::nullopt;
                            auto auth = SecurityHelper::authorizeWrite(
                                *security_manager_, security::Protocol::WebSocket, ip, db, "", origin, headers);
                            if (auth.hasError())
                                return auth.error();
                            return std::nullopt;
                        },
                        [&](uint16_t db, size_t offset, size_t size, const uint8_t* data) {
                            return binary_write_fn_(db, offset, size, data);
                        });
                    write_error.has_value()) {
                    ok = false;
                    error = std::move(*write_error);
                }
            }

            sendText(t_conn, serializeWriteAck(sequence, ok, error));
        } else if (cmd == "write_area" && doc.hasMember("updates") && doc["updates"].is_array()) {
            // Area writes share record validation and base64 handling with DB
            // writes, but intentionally keep the no-DB authorization context.
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

            std::optional<uint64_t> sequence;
            if (doc.hasMember("sequence") && doc["sequence"].isUint64())
                sequence = doc["sequence"].getUint64();

            if (area_ok) {
                if (auto write_error = applyRuntimeSyncWrites(
                        doc["updates"].elements(),
                        [](const sgrn::json::dom::Value& item) -> std::optional<runtime_sync_json::AreaWriteRecord> {
                            if (!item.is_object() || !item.hasMember("area") || !item["area"].isUint() || item["area"].getUint() > 65535 ||
                                !item.hasMember("offset") || !item["offset"].isUint64() || !item.hasMember("size") ||
                                !item["size"].isUint64() || !item.hasMember("data") || !item["data"].is_string())
                                return std::nullopt;
                            runtime_sync_json::AreaWriteRecord record;
                            record.area = item["area"].getUint();
                            record.offset = item["offset"].getUint64();
                            record.size = item["size"].getUint64();
                            record.data = item["data"].getString();
                            return record;
                        },
                        "invalid RuntimeSync area write record", "RuntimeSync area write range is invalid",
                        "RuntimeSync area write payload is shorter than size",
                        [](const runtime_sync_json::AreaWriteRecord& item) { return item.area; },
                        [&](uint16_t) -> std::optional<std::string> {
                            if (!security_manager_)
                                return std::nullopt;
                            auto auth = SecurityHelper::authorizeWrite(
                                *security_manager_, security::Protocol::WebSocket, ip, std::nullopt, "", origin, headers);
                            if (auth.hasError())
                                return auth.error();
                            return std::nullopt;
                        },
                        [&](uint16_t area, size_t offset, size_t size, const uint8_t* data) {
                            return area_write_fn(area, offset, size, data);
                        });
                    write_error.has_value()) {
                    area_ok = false;
                    area_error = std::move(*write_error);
                }
            }

            sendText(t_conn, serializeWriteAck(sequence, area_ok, area_error));
        } else if (cmd == "subscribe" && doc.hasMember("path") && doc["path"].is_string()) {
            std::string path = doc["path"].getString();
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
        } else if (cmd == "subscribe_binary" && doc.hasMember("db")) {
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
            if (doc.hasMember("offset")) {
                if (!doc["offset"].isUint64()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: invalid offset", *db_num);
                    return;
                }
                auto offset_raw = doc["offset"].getUint64();
                if (offset_raw > std::numeric_limits<size_t>::max()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: offset out of range", *db_num);
                    return;
                }
                offset = static_cast<size_t>(offset_raw);
            }

            std::optional<size_t> size_opt;
            if (doc.hasMember("size")) {
                if (!doc["size"].isUint64()) {
                    SGRN_WARN_LOG("WebSocket binary subscribe from DB{} rejected: invalid size", *db_num);
                    return;
                }
                auto size_raw = doc["size"].getUint64();
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
        } else if (cmd == "unsubscribe" && doc.hasMember("path") && doc["path"].is_string()) {
            std::string path = doc["path"].getString();
            std::lock_guard<std::mutex> lk(clients_mutex_);
            auto it = clients_.find(&t_conn);
            if (it != clients_.end()) {
                it->second.subscriptions.erase(path);
                resolveLeafRanges(it->second);
            }
        } else if (cmd == "unsubscribe_binary" && doc.hasMember("db")) {
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

            const bool has_offset = doc.hasMember("offset");
            const bool has_size = doc.hasMember("size");
            if (!has_offset && !has_size) {
                auto& subs = it->second.binary_subscriptions;
                subs.erase(std::remove_if(
                               subs.begin(), subs.end(), [&](const ClientContext::BinarySubscription& sub) { return sub.db == *db_num; }),
                    subs.end());
            } else if (has_offset && has_size && doc["offset"].isUint64() && doc["size"].isUint64()) {
                auto offset_raw = doc["offset"].getUint64();
                auto size_raw = doc["size"].getUint64();
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
            const bool enabled = doc.hasMember("enabled") && doc["enabled"].is_boolean() && doc["enabled"].getBool();
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

    Frame ack{.kind = Kind::Ack, .sequence = frame.sequence, .timestamp_ms = frame.timestamp_ms, .records = {}};

    std::string encoded = encode(ack);
    if (ok && !encoded.empty()) {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        if (clients_.find(&t_conn) != clients_.end()) {
            sendBinary(t_conn, encoded.data(), encoded.size());
        }
    } else {
        SGRN_WARN_LOG("RuntimeSync binary write failed: {}", error);
    }
}

} // namespace sgrn::gateway::adapters::websocket
