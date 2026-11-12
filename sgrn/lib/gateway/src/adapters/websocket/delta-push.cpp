#include <sgrn/utils/json-dom.hpp>
// WebSocketAdapter outbound push path (PLC -> clients).
// Split from WebSocketAdapter.cpp; mirrors adapters/opcua/delta_push.cpp.
#include <fmt/core.h>
#include <sgrn/common/endian-helper.hpp>
#include <sgrn/common/event-helper.hpp>
#include <sgrn/common/path-utils.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/common/SchemaResolver.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/json-ops.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

// Leaf ranges are sorted and coalesced by resolveLeafRanges(). Find the first
// range whose end can contain the id instead of scanning every range.
template <class Ranges>
bool containsLeafId(const Ranges& t_ranges, twin::LeafId t_id) noexcept {
    const auto it = std::lower_bound(
        t_ranges.begin(), t_ranges.end(), t_id, [](const auto& t_range, twin::LeafId t_value) { return t_range.end < t_value; });
    return it != t_ranges.end() && it->start <= t_id;
}

// Flat dictionary payloads have decimal leaf IDs as keys. Parse without
// std::stoul: malformed wire keys must not throw out of the broker callback.
std::optional<twin::LeafId> parseLeafId(std::string_view t_text) noexcept {
    if (t_text.empty() || t_text.size() > 10)
        return std::nullopt;

    uint64_t value = 0;
    constexpr uint64_t max_value = std::numeric_limits<twin::LeafId>::max();
    for (const unsigned char ch : t_text) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (digit > max_value || value > (max_value - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    return static_cast<twin::LeafId>(value);
}

// Serialize only the selected entries of a flat dictionary document. Building
// a second DOM would deep-copy every selected subtree and then walk it again
// to serialize; direct fragment serialization avoids that intermediate tree.
template <class Ranges>
std::optional<std::string> filterFlatDictionaryJson(const sgrn::json::dom::Document& t_doc, const Ranges& t_ranges) {
    std::string output;
    output.reserve(128);
    output.push_back('{');
    bool first = true;
    std::string value_json;

    for (auto it = t_doc.memberBegin(); it != t_doc.memberEnd(); ++it) {
        const std::string_view key{it->name.getString(), it->name.getStringLength()};
        const auto id = parseLeafId(key);
        if (!id || !containsLeafId(t_ranges, *id))
            continue;

        const auto& value = static_cast<const sgrn::json::dom::Value&>(it->value);
        const auto serialized = sgrn::json::writeTo(value_json, value.rawValue());
        if (serialized.hasError())
            return std::nullopt;

        if (!first)
            output.push_back(',');
        first = false;
        output.push_back('"');
        output.append(key); // parseLeafId guarantees a decimal-only JSON key
        output.append(R"(":)");
        output.append(value_json);
    }

    if (first)
        return std::string{};
    output.push_back('}');
    return output;
}

} // namespace

void WebSocketAdapter::handleTelemetryEvent(const TelemetryEvent& t_event) {
    RETURN_IF(t_event.type != EventType::DeltaSnapshot, ;);

    // Parse once into TelemetryEvent's shared lazy DOM. The last-value cache
    // and any filtered subscribers reuse this same parse instead of parsing
    // the flat payload independently.
    const sgrn::json::dom::Document* parsed_doc = nullptr;
    if (t_event.is_flat && t_event.json_value && !t_event.json_value->empty() && *t_event.json_value != "{}") {
        parsed_doc = t_event.parsedJson();
        if (parsed_doc)
            rememberFlatValues(*parsed_doc);
    }

    // Collect target clients and determine if any needs field-level filtering
    bool t_any_needs_filter = false;
    auto targets = collectTargets(t_event, t_any_needs_filter);
    // Binary targets are keyed by DB number, but DeltaSnapshot events carry
    // the dirty DB list (dirty_dbs) rather than a single db — collect across
    // all of them so binary subscribers actually receive frames.
    std::map<std::tuple<uint16_t, size_t, size_t>, std::vector<crow::websocket::connection*>> binary_targets;
    if (!t_event.dirty_dbs.empty()) {
        for (uint16_t dirty_db : t_event.dirty_dbs) {
            for (auto& [key, conns] : collectBinaryTargets(dirty_db)) {
                auto& vec = binary_targets[key];
                vec.insert(vec.end(), conns.begin(), conns.end());
            }
        }
    } else {
        binary_targets = collectBinaryTargets(t_event.db);
    }

    RETURN_IF(targets.empty() && binary_targets.empty(), ;);

    // ── PERFORMANCE NOTE: Shared JSON Parsing ─────────────────────────────────
    // Flat snapshots are parsed once for the last-value cache (needed to seed
    // late dictionary-mode subscribers); filtered clients reuse that same
    // TelemetryEvent DOM instead of parsing the payload again. Firehose sends
    // still forward the original shared JSON string without building a
    // per-client output DOM. Non-flat events are parsed only when filtering
    // actually requires inspection.
    // ──────────────────────────────────────────────────────────────────────────

    // Persistence, the last-value cache, and filtered clients share the same
    // lazy document.
    if (t_any_needs_filter && !parsed_doc)
        parsed_doc = t_event.parsedJson();

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
                        auto filtered = filterFlatDictionaryJson(*parsed_doc, target.leaf_ranges);
                        if (filtered && !filtered->empty())
                            text_sends.emplace_back(target.conn, std::make_shared<const std::string>(std::move(*filtered)));
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
                    sgrn::json::dom::Document flat_doc;
                    if (!twin::flattenNestedTree(*parsed_doc, dict_->path_to_id, flat_doc.allocator(), flat_doc).hasError()) {
                        if (!target.leaf_ranges.empty()) {
                            auto filtered = filterFlatDictionaryJson(flat_doc, target.leaf_ranges);
                            if (filtered && !filtered->empty())
                                text_sends.emplace_back(target.conn, std::make_shared<const std::string>(std::move(*filtered)));
                        } else {
                            std::string flat_json;
                            const auto serialized = sgrn::json::writeTo(flat_json, flat_doc.rawValue());
                            if (!serialized.hasError())
                                text_sends.emplace_back(target.conn, std::make_shared<const std::string>(std::move(flat_json)));
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
            // field_subs are normalized to dotted PLC paths when targets are
            // collected; do not rebuild the set for every event.
            auto payload = sgrn::utils::json::filterFields(*parsed_doc, target.field_subs);
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

    RETURN_IF(binary_targets.empty(), ;);

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
        storeToBuffer<uint32_t>(static_cast<uint32_t>(db), frame.data(), s7codec::Endian::Little);
        storeToBuffer<double>(timestamp_seconds, frame.data() + 4, s7codec::Endian::Little);

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

    // Convert dirty TreePaths lazily: firehose / DB-level-only events do not
    // need dotted strings at all. Once a field filter needs them, share the
    // conversion across every client and subscription for this event.
    std::vector<std::string> dirty_dotted;
    bool dirty_dotted_ready = false;

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
            if (!dirty_dotted_ready) {
                dirty_dotted.reserve(t_event.dirty_paths.size());
                for (const auto& dirty : t_event.dirty_paths)
                    dirty_dotted.push_back(dirty.toDotted());
                dirty_dotted_ready = true;
            }

            // Collect the field-level subscriptions that overlap dirty paths.
            for (const auto& sub : subs) {
                if (sub.find('/') == std::string::npos)
                    continue; // DB-level subscription — not field-level
                std::string sub_dotted = path_utils::topicToPlcPath(sub);
                for (const auto& dirty_str : dirty_dotted) {
                    // Require a dot boundary so "React" does not accidentally
                    // match "ReactorCore"; avoid allocating prefix + "." strings.
                    const bool exact_or_sub =
                        dirty_str == sub_dotted ||
                        (dirty_str.size() > sub_dotted.size() && dirty_str.starts_with(sub_dotted) &&
                            dirty_str[sub_dotted.size()] == '.') ||
                        (sub_dotted.size() > dirty_str.size() && sub_dotted.starts_with(dirty_str) && sub_dotted[dirty_str.size()] == '.');
                    if (exact_or_sub) {
                        // Store the dotted form consumed by filterFields().
                        t_field_subs.insert(std::move(sub_dotted));
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
    RETURN_IF_NULL(tp_conn, false);

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
    storeToBuffer<uint32_t>(static_cast<uint32_t>(t_db), frame.data(), s7codec::Endian::Little);
    storeToBuffer<double>(t_timestamp_seconds, frame.data() + 4, s7codec::Endian::Little);

    auto read_res = binary_read_fn_(t_db, t_offset, t_size, frame.data() + 12);
    if (read_res.hasError()) {
        SGRN_WARN_LOG("WebSocket binary read for DB{} offset {} size {} failed: {}", t_db, t_offset, t_size, read_res.error());
        return false;
    }

    std::lock_guard<std::mutex> lk(clients_mutex_);
    RETURN_IF(clients_.find(tp_conn) == clients_.end(), true); // disconnected while we were reading; not an error
    sendBinary(*tp_conn, frame.data(), frame.size());
    return true;
}

void WebSocketAdapter::rememberFlatValues(const sgrn::json::dom::Document& t_flat_doc) {
    if (!t_flat_doc.is_object())
        return;

    // Serialize outside last_values_mutex_: catch-up readers should not wait
    // for every changed leaf to be encoded. Commit all updates under one lock.
    std::vector<std::pair<twin::LeafId, std::string>> updates;
    updates.reserve(t_flat_doc.memberCount());

    std::string value_json;
    for (auto it = t_flat_doc.memberBegin(); it != t_flat_doc.memberEnd(); ++it) {
        const std::string_view key{it->name.getString(), it->name.getStringLength()};
        const auto id = parseLeafId(key);
        if (!id)
            continue;

        const auto& value = static_cast<const sgrn::json::dom::Value&>(it->value);
        const auto serialized = sgrn::json::writeTo(value_json, value.rawValue());
        if (serialized.hasError())
            continue;

        updates.emplace_back(*id, value_json);
    }

    if (updates.empty())
        return;

    std::lock_guard<std::mutex> lock(last_values_mutex_);
    for (auto& [id, json_str] : updates)
        last_flat_values_.insert_or_assign(id, std::move(json_str));
}

void WebSocketAdapter::sendCatchUp(crow::websocket::connection& t_conn, const std::vector<ClientContext::LeafRange>& t_ranges) {
    RETURN_IF(t_ranges.empty(), ;);

    // Values were serialized when rememberFlatValues() populated the cache.
    // Build the numeric-keyed object by splicing those trusted fragments;
    // reparsing every value into a temporary Document is unnecessary.
    std::string payload;
    {
        std::lock_guard<std::mutex> lk(last_values_mutex_);
        RETURN_IF(last_flat_values_.empty(), ;);

        size_t reserve_size = 2;
        constexpr size_t kLeafIdMaxDigits = std::numeric_limits<twin::LeafId>::digits10 + 1;
        for (const auto& [id, value_json] : last_flat_values_) {
            if (containsLeafId(t_ranges, id))
                reserve_size += kLeafIdMaxDigits + value_json.size() + 4;
        }
        payload.reserve(reserve_size);
        payload.push_back('{');
        bool first = true;
        for (const auto& [id, value_json] : last_flat_values_) {
            if (!containsLeafId(t_ranges, id))
                continue;
            if (!first)
                payload.push_back(',');
            first = false;
            payload.push_back('"');
            payload.append(std::to_string(id));
            payload.append(R"(":)");
            payload.append(value_json);
        }
        payload.push_back('}');
    }

    RETURN_IF(payload == "{}", ;);
    std::lock_guard<std::mutex> lk(clients_mutex_);
    RETURN_IF(clients_.find(&t_conn) == clients_.end(), ;); // disconnected while we were assembling the catch-up
    sendText(t_conn, payload);
}

void WebSocketAdapter::resolveLeafRanges(ClientContext& t_ctx) {
    t_ctx.leaf_ranges.clear();
    RETURN_IF(!dict_ || dict_->path_to_id.empty() || !t_ctx.dictionary_mode, ;);

    // path_by_id is already ordered by LeafId. Scan it as string_views and
    // coalesce matching runs as we go, avoiding a map traversal plus
    // temporary DB/field strings and one {id,id} allocation per leaf.
    for (const auto& sub : t_ctx.subscriptions) {
        const std::string dotted = path_utils::topicToPlcPath(sub);
        const size_t sub_dot = dotted.find('.');
        const std::string_view sub_db = sub_dot == std::string::npos ? std::string_view{dotted} : std::string_view{dotted.data(), sub_dot};
        const std::string_view sub_field =
            sub_dot == std::string::npos ? std::string_view{} : std::string_view{dotted.data() + sub_dot + 1, dotted.size() - sub_dot - 1};

        bool in_run = false;
        ClientContext::LeafRange run{};
        for (size_t index = 0; index < dict_->path_by_id.size(); ++index) {
            const std::string& path = dict_->path_by_id[index];
            const size_t leaf_dot = path.find('.');
            const std::string_view leaf_db =
                leaf_dot == std::string::npos ? std::string_view{path} : std::string_view{path.data(), leaf_dot};
            const std::string_view leaf_field = leaf_dot == std::string::npos
                                                    ? std::string_view{}
                                                    : std::string_view{path.data() + leaf_dot + 1, path.size() - leaf_dot - 1};

            bool matches = leaf_db == sub_db;
            if (matches && !sub_field.empty()) {
                matches = leaf_field == sub_field || (leaf_field.size() > sub_field.size() && leaf_field.starts_with(sub_field) &&
                                                         leaf_field[sub_field.size()] == '.');
            }

            const auto id = static_cast<twin::LeafId>(index);
            if (matches) {
                if (in_run && run.end != std::numeric_limits<twin::LeafId>::max() && id == run.end + 1) {
                    run.end = id;
                } else {
                    if (in_run)
                        t_ctx.leaf_ranges.push_back(run);
                    run = {id, id};
                    in_run = true;
                }
            } else if (in_run) {
                t_ctx.leaf_ranges.push_back(run);
                in_run = false;
            }
        }
        if (in_run)
            t_ctx.leaf_ranges.push_back(run);
    }

    // Merge overlaps/adjacent runs produced by multiple subscriptions.
    if (t_ctx.leaf_ranges.size() > 1) {
        std::sort(t_ctx.leaf_ranges.begin(), t_ctx.leaf_ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        std::vector<ClientContext::LeafRange> merged;
        merged.reserve(t_ctx.leaf_ranges.size());
        merged.push_back(t_ctx.leaf_ranges.front());
        for (size_t i = 1; i < t_ctx.leaf_ranges.size(); ++i) {
            auto& back = merged.back();
            if (back.end != std::numeric_limits<twin::LeafId>::max() && t_ctx.leaf_ranges[i].start <= back.end + 1) {
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
    RETURN_IF(broker_sub_id_ == 0, ;);
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& [conn, ctx] : clients_) {
        (void)ctx;
        if (conn)
            sendText(*conn, t_json_snapshot);
    }
}

} // namespace sgrn::gateway::adapters::websocket
