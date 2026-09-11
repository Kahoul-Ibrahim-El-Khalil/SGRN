// WebSocketAdapter outbound push path (PLC -> clients).
// Split from WebSocketAdapter.cpp; mirrors adapters/opcua/delta_push.cpp.
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

void WebSocketAdapter::handleTelemetryEvent(const TelemetryEvent& t_event) {
    if (t_event.type != EventType::DeltaSnapshot)
        return;

    // Feed the last-value cache so late subscribers get current state even
    // when no further deltas arrive (quiet twin, finished replay).
    if (t_event.is_flat && t_event.json_value && !t_event.json_value->empty() && *t_event.json_value != "{}") {
        rememberFlatValues(*t_event.json_value);
    }

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
    // Shared lazy DOM: at most one parse per event across ALL subscribers
    // (persistence shares it); null when the payload is not a JSON object,
    // in which case every client below falls back to the unfiltered send.
    const rapidjson::Document* parsed_doc = t_any_needs_filter ? t_event.parsedJson() : nullptr;

    // Build (connection, payload) pairs first; payloads are shared — the
    // firehose paths below alias the broker's own string (zero copies until
    // Crow's send), filtered paths allocate once. The actual sends happen
    // under clients_mutex_ with a membership re-check (see sendText notes in
    // the header: a connection is only touched while registered).
    std::vector<std::pair<crow::websocket::connection*, std::shared_ptr<const std::string>>> text_sends;

    // Send to each client — either full JSON (zero overhead) or filtered.
    for (auto& target : targets) {
        if (target.dictionary_mode && dict_ && !dict_->path_to_id.empty()) {
            // ── Fast path: event is already a flat {"<id>": value} blob ────────
            // getDeltaSnapshotFlat() ran on the server side, so there is nothing
            // to parse or re-serialize for firehose dictionary clients.
            // For range-filtered clients we must parse once and filter by id.
            if (t_event.is_flat) {
                if (target.leaf_ranges.empty()) {
                    // Firehose dictionary mode: zero-copy send of the flat blob.
                    text_sends.emplace_back(target.conn, t_event.json_value);
                } else {
                    // Subscription-filtered: parse once and emit only matching ids.
                    if (!parsed_doc)
                        parsed_doc = t_event.parsedJson();
                    if (parsed_doc) {
                        rapidjson::Document filtered_doc;
                        filtered_doc.SetObject();
                        for (auto it = parsed_doc->MemberBegin(); it != parsed_doc->MemberEnd(); ++it) {
                            twin::LeafId id = static_cast<twin::LeafId>(std::stoul(it->name.GetString()));
                            for (const auto& range : target.leaf_ranges) {
                                if (id >= range.start && id <= range.end) {
                                    rapidjson::Value key, val;
                                    key.CopyFrom(it->name, filtered_doc.GetAllocator());
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
                            text_sends.emplace_back(target.conn, std::make_shared<const std::string>(filtered_sb.GetString()));
                    } else {
                        text_sends.emplace_back(target.conn, t_event.json_value);
                    }
                }
            } else {
                // ── Legacy path: nested blob — parse + flatten on demand ─────
                // This executes only when the gateway has no LeafDictionary
                // configured (pre-Phase-4 deployment or non-schema mode).
                if (!parsed_doc)
                    parsed_doc = t_event.parsedJson();
                if (parsed_doc) {
                    rapidjson::Document flat_doc;
                    if (!twin::flattenNestedTree(*parsed_doc, dict_->path_to_id, flat_doc.GetAllocator(), flat_doc).hasError()) {
                        if (!target.leaf_ranges.empty()) {
                            rapidjson::Document filtered_doc;
                            filtered_doc.SetObject();
                            for (auto it = flat_doc.MemberBegin(); it != flat_doc.MemberEnd(); ++it) {
                                twin::LeafId id = static_cast<twin::LeafId>(std::stoul(it->name.GetString()));
                                for (const auto& range : target.leaf_ranges) {
                                    if (id >= range.start && id <= range.end) {
                                        rapidjson::Value key, val;
                                        key.CopyFrom(it->name, filtered_doc.GetAllocator());
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
                                text_sends.emplace_back(target.conn, std::make_shared<const std::string>(filtered_sb.GetString()));
                        } else {
                            rapidjson::StringBuffer flat_sb;
                            rapidjson::Writer<rapidjson::StringBuffer> flat_w(flat_sb);
                            flat_doc.Accept(flat_w);
                            text_sends.emplace_back(target.conn, std::make_shared<const std::string>(flat_sb.GetString()));
                        }
                    } else {
                        text_sends.emplace_back(target.conn, t_event.json_value);
                    }
                } else {
                    text_sends.emplace_back(target.conn, t_event.json_value);
                }
            }
        } else if (target.needs_filter && parsed_doc) {
            // Non-dictionary filtered client: legacy path-based filtering
            std::set<std::string> dotted_subs;
            for (const auto& sub : target.field_subs) {
                dotted_subs.insert(path_utils::topicToPlcPath(sub));
            }
            auto payload = json_helper::filterFields(*parsed_doc, dotted_subs);
            if (!payload.empty() && payload != "{}")
                text_sends.emplace_back(target.conn, std::make_shared<const std::string>(std::move(payload)));
        } else {
            text_sends.emplace_back(target.conn, t_event.json_value);
        }
    }

    if (!text_sends.empty()) {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto& [conn, payload] : text_sends) {
            if (!payload || clients_.find(conn) == clients_.end())
                continue; // disconnected while we were filtering
            sendText(*conn, *payload);
        }
    }

    if (binary_targets.empty())
        return;

    const double timestamp_seconds = static_cast<double>(t_event.timestamp) / 1000.0;
    struct BinarySend {
        std::vector<uint8_t> frame;
        std::vector<crow::websocket::connection*> conns;
    };
    std::vector<BinarySend> binary_sends;
    for (auto& [key, conns] : binary_targets) {
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
        binary_sends.push_back({std::move(frame), std::move(conns)});
    }

    if (!binary_sends.empty()) {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto& send : binary_sends) {
            for (auto* conn : send.conns) {
                if (!conn || clients_.find(conn) == clients_.end())
                    continue; // disconnected while we were reading
                sendBinary(*conn, send.frame.data(), send.frame.size());
            }
        }
    }
}

std::vector<WebSocketAdapter::TargetInfo> WebSocketAdapter::collectTargets(const TelemetryEvent& t_event, bool& t_any_needs_filter) {

    std::vector<TargetInfo> targets;
    t_any_needs_filter = false;

    // Map membership implies an open connection: entries are added in onopen
    // and removed in onclose. Sends re-validate under the same mutex.
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& [conn, ctx] : clients_) {
        const auto& subs = ctx.subscriptions;

        // Common event_filter::shouldSend — firehose mode or path overlap
        bool send = event_filter::shouldSend(t_event.dirty_paths, subs);

        if (!send)
            continue;

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
                    const bool exact_or_sub =
                        dirty_str == sub_dotted || dirty_str.starts_with(sub_dotted + ".") || sub_dotted.starts_with(dirty_str + ".");
                    if (exact_or_sub) {
                        t_field_subs.insert(sub);
                        break;
                    }
                }
            }
        }

        if (needs_filter) {
            t_any_needs_filter = true;
            targets.push_back({conn, true, ctx.dictionary_mode, std::move(t_field_subs), ctx.leaf_ranges});
        } else {
            targets.push_back({conn, false, ctx.dictionary_mode, {}, ctx.leaf_ranges});
        }
    }

    return targets;
}

std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<crow::websocket::connection*>> WebSocketAdapter::collectBinaryTargets(
    uint16_t t_db) {
    std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<crow::websocket::connection*>> targets;

    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& [conn, ctx] : clients_) {
        for (const auto& sub : ctx.binary_subscriptions) {
            if (sub.db != t_db)
                continue;
            targets[{sub.db, sub.offset, sub.size}].push_back(conn);
        }
    }

    return targets;
}

bool WebSocketAdapter::sendBinaryFrame(
    crow::websocket::connection* tp_conn, uint16_t t_db, size_t t_offset, size_t t_size, double t_timestamp_seconds) {
    if (!tp_conn)
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

    std::lock_guard<std::mutex> lk(clients_mutex_);
    if (clients_.find(tp_conn) == clients_.end())
        return true; // disconnected while we were reading; not an error
    sendBinary(*tp_conn, frame.data(), frame.size());
    return true;
}

void WebSocketAdapter::rememberFlatValues(const std::string& t_flat_json) {
    rapidjson::Document doc;
    doc.Parse(t_flat_json.c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return;

    std::lock_guard<std::mutex> lk(last_values_mutex_);
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const char* name = it->name.GetString();
        size_t len = it->name.GetStringLength();
        if (len == 0 || len > 10)
            continue;
        bool numeric = true;
        for (size_t i = 0; i < len; ++i) {
            if (name[i] < '0' || name[i] > '9') {
                numeric = false;
                break;
            }
        }
        if (!numeric)
            continue;
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        it->value.Accept(w);
        last_flat_values_[static_cast<twin::LeafId>(std::stoul(name))] = sb.GetString();
    }
}

void WebSocketAdapter::sendCatchUp(crow::websocket::connection& t_conn, const std::vector<ClientContext::LeafRange>& t_ranges) {
    if (t_ranges.empty())
        return;

    rapidjson::Document out;
    out.SetObject();
    {
        std::lock_guard<std::mutex> lk(last_values_mutex_);
        if (last_flat_values_.empty())
            return;
        for (const auto& [id, value_json] : last_flat_values_) {
            bool covered = false;
            for (const auto& range : t_ranges) {
                if (id >= range.start && id <= range.end) {
                    covered = true;
                    break;
                }
            }
            if (!covered)
                continue;
            rapidjson::Document value_doc;
            value_doc.Parse(value_json.c_str());
            if (value_doc.HasParseError())
                continue;
            rapidjson::Value key(std::to_string(id).c_str(), out.GetAllocator());
            rapidjson::Value val;
            val.CopyFrom(value_doc, out.GetAllocator());
            out.AddMember(key, val, out.GetAllocator());
        }
    }

    if (out.ObjectEmpty())
        return;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    out.Accept(w);
    std::lock_guard<std::mutex> lk(clients_mutex_);
    if (clients_.find(&t_conn) == clients_.end())
        return; // disconnected while we were assembling the catch-up
    sendText(t_conn, sb.GetString());
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

void WebSocketAdapter::broadcastDelta(const std::string& t_json_snapshot, uint64_t t_timestamp_ms) {
    (void)t_timestamp_ms;
    // Live while subscribed to the broker (set by start()/registerRoutes(),
    // cleared by stop()) — independent of who owns the listener.
    if (broker_sub_id_ == 0)
        return;
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& [conn, ctx] : clients_) {
        (void)ctx;
        if (conn)
            sendText(*conn, t_json_snapshot);
    }
}

} // namespace sgrn::gateway::adapters::websocket
