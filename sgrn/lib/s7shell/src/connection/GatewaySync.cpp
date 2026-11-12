#include <sgrn/s7shell/connection/GatewaySync.hpp>

#include <sgrn/gateway/adapters/websocket/RuntimeSyncProtocol.hpp>
#include <sgrn/gateway/common/JsonLeafVisitor.hpp>
#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json-serialization.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <httplib.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::s7shell::connection
{
thread_local bool GatewaySync::suppress_publish_ = false;

// The shared RuntimeSync protocol header owns the reflected JSON DTOs used
// by both this client and the gateway's WebSocket write handler.
namespace
{
namespace runtime_sync_json = ::sgrn::gateway::adapters::websocket::runtime_sync::json;
using SyncBatchItemJson = runtime_sync_json::DbWriteRecord;
using SyncBatchEnvelopeJson = runtime_sync_json::DbWriteEnvelope;
using SyncAreaItemJson = runtime_sync_json::AreaWriteRecord;
using SyncAreaEnvelopeJson = runtime_sync_json::AreaWriteEnvelope;
using DictionaryLeafJson = runtime_sync_json::DictionaryLeaf;
using DictionaryFrameJson = runtime_sync_json::DictionaryFrame;
using WriteAckJson = runtime_sync_json::WriteAck;
using TypeProbeJson = runtime_sync_json::TypeProbe;

std::string jsonString(const std::string& t_value) {
    // sgrn::json::write of a bare string yields the quoted, escaped JSON form.
    auto res = sgrn::json::write(t_value);
    return res.value_or(std::string{});
}

std::string websocketUrlToHttpBase(std::string t_url) {
    if (t_url.starts_with("ws://"))
        t_url.replace(0, 5, "http://");
    else if (t_url.starts_with("wss://"))
        t_url.replace(0, 6, "https://");

    const auto scheme_pos = t_url.find("://");
    const auto path_pos = t_url.find('/', scheme_pos == std::string::npos ? 0 : scheme_pos + 3);
    if (path_pos != std::string::npos)
        t_url.resize(path_pos);
    return t_url;
}

} // namespace

GatewaySync::GatewaySync(PlcRuntimeSPtr tsp_runtime)
    : runtime_(std::move(tsp_runtime)) {
    // Debounce knob for fast-paced captures: SGRN_SYNC_COALESCE_MS lowers
    // the window so high-rate simulators keep per-tick WAL samples.
    if (const char* ev = std::getenv("SGRN_SYNC_COALESCE_MS"); ev != nullptr && *ev != '\0') {
        const uint32_t parsed = static_cast<uint32_t>(std::atoi(ev));
        publish_coalesce_ms_ = std::clamp(parsed, 0u, 10'000u);
    }
    if (runtime_) {
        dirty_observer_id_ =
            runtime_->addDirtyObserver([this](uint16_t t_db, uint32_t offset, uint32_t length) { onRuntimeDirty(t_db, offset, length); });
        tag_observer_id_ = runtime_->addTagDirtyObserver([this](const std::string& t_name) { onTagDirty(t_name); });
    }
    publish_worker_ = std::thread([this]() { publishWorkerLoop(); });
}

GatewaySync::~GatewaySync() {
    if (runtime_) {
        if (dirty_observer_id_ != 0)
            runtime_->removeDirtyObserver(dirty_observer_id_);
        if (tag_observer_id_ != 0)
            runtime_->removeTagDirtyObserver(tag_observer_id_);
    }
    disconnect();
    {
        std::lock_guard<std::mutex> lk(publish_mutex_);
        stop_publish_worker_ = true;
        publish_requested_ = true;
    }
    publish_cv_.notify_one();
    if (publish_worker_.joinable())
        publish_worker_.join();
}

void GatewaySync::subscribeDb(uint16_t t_db) {
    {
        std::lock_guard<std::mutex> lk(subs_mutex_);
        subscribed_dbs_.insert(t_db);
        // Any explicit subscription cancels receive-mute (see unsubscribeDb).
        mute_when_empty_ = false;
    }
    if (connected_ && runtime_) {
        auto schema = runtime_->getSchema().getDb(t_db);
        if (!schema.hasError() && schema.value()) {
            const std::string command = R"({"command":"subscribe","path":)" + jsonString(schema.value()->db_name) + "}";
            (void)ws_.sendText(command);
        }
    }
}

void GatewaySync::unsubscribeDb(uint16_t t_db) {
    bool now_muted = false;
    {
        std::lock_guard<std::mutex> lk(subs_mutex_);
        subscribed_dbs_.erase(t_db);
        // Unsubscribing everything must NOT leave the server in firehose mode
        // (empty subscription set = "send everything" per event_filter): a
        // publish-only client would then receive its own writes back and
        // re-apply them, churning the dirty ledger and regressing memory with
        // stale echoes under load. Hold one reserved, never-matching
        // subscription instead so the server sends nothing.
        if (subscribed_dbs_.empty()) {
            mute_when_empty_ = true;
            now_muted = true;
        }
    }
    if (connected_ && runtime_) {
        auto schema = runtime_->getSchema().getDb(t_db);
        if (!schema.hasError() && schema.value()) {
            const std::string command = R"({"command":"unsubscribe","path":)" + jsonString(schema.value()->db_name) + "}";
            (void)ws_.sendText(command);
        }
        if (now_muted) {
            const std::string mute = R"({"command":"subscribe","path":)" + jsonString(kMutedPath) + "}";
            (void)ws_.sendText(mute);
        }
    }
}

void GatewaySync::publishOnDirty(bool t_enabled) {
    publish_on_dirty_ = t_enabled;
    if (t_enabled)
        requestPublish();
}

void GatewaySync::useBinary(bool t_enabled) {
    binary_transport_ = t_enabled;
}

bool GatewaySync::isDbSubscribed(uint16_t t_db) const {
    std::lock_guard<std::mutex> lk(subs_mutex_);
    return subscribed_dbs_.empty() || subscribed_dbs_.contains(t_db);
}

std::string GatewaySync::getLastError() const {
    std::lock_guard<std::mutex> lk(err_mutex_);
    return last_error_;
}

bool GatewaySync::connect(const std::string& t_ws_url) {
    http_base_url_ = websocketUrlToHttpBase(t_ws_url);
    // Local dictionary fallback for flat deltas from servers that never
    // push the {"type":"dictionary"} frame. The remote frame, when present,
    // always wins (see resolveLeafPath).
    if (runtime_) {
        std::lock_guard<std::mutex> lk(dict_mutex_);
        local_dict_ = ::sgrn::gateway::twin::LeafDictionary::buildFrom(runtime_->getSchema());
        local_dict_built_ = true;
    }
    ws_.setUrl(t_ws_url);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& t_msg) { onMessage(t_msg); });
    ws_.start();
    fmt::print(fg(fmt::color::cyan), "[GatewayBinding] Connecting to {}...\n", t_ws_url);
    return true;
}

void GatewaySync::disconnect() {
    ws_.stop();
    connected_ = false;
}

void GatewaySync::onMessage(const ix::WebSocketMessagePtr& t_msg) {
    switch (t_msg->type) {
        case ix::WebSocketMessageType::Open:
            connected_ = true;
            if (runtime_) {
                std::set<uint16_t> selected;
                bool muted = false;
                {
                    std::lock_guard<std::mutex> lk(subs_mutex_);
                    selected = subscribed_dbs_;
                    muted = mute_when_empty_;
                }
                if (selected.empty() && muted) {
                    // Receive-muted (see unsubscribeDb): hold the reserved
                    // non-matching subscription instead of firehose.
                    const std::string mute = R"({"command":"subscribe","path":)" + jsonString(kMutedPath) + "}";
                    (void)ws_.sendText(mute);
                } else {
                    for (const auto& [db_num, db_schema] : runtime_->getSchema().dbs()) {
                        if (!selected.empty() && !selected.contains(db_num))
                            continue;
                        const std::string command = R"({"command":"subscribe","path":)" + jsonString(db_schema.db_name) + "}";
                        (void)ws_.sendText(command);
                    }
                }
            }
            requestPublish();
            fmt::print(fg(fmt::color::green), "[GatewayBinding] Connected.\n");
            break;
        case ix::WebSocketMessageType::Close:
            connected_ = false;
            break;
        case ix::WebSocketMessageType::Error: {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = t_msg->errorInfo.reason;
            fmt::print(stderr, fg(fmt::color::red), "[GatewayBinding] Error: {}\n", t_msg->errorInfo.reason);
            break;
        }
        case ix::WebSocketMessageType::Message:
            if (t_msg->binary) {
                // RuntimeSync acknowledgements are transport-level metadata;
                // telemetry remains JSON for schema-aware field updates.
                ::sgrn::gateway::adapters::websocket::runtime_sync::Frame frame;
                if (::sgrn::gateway::adapters::websocket::runtime_sync::decode(t_msg->str, frame) &&
                    frame.kind == ::sgrn::gateway::adapters::websocket::runtime_sync::Kind::Ack) {
                    {
                        std::lock_guard<std::mutex> lk(ack_mutex_);
                        last_ack_sequence_ = frame.sequence;
                        last_ack_ok_ = true;
                    }
                    ack_cv_.notify_all();
                    return;
                }
                return;
            }
            // ── Text frame dispatch ────────────────────────────────────
            // DeltaSnapshot payloads have NO "type" key: {"<leaf-id>": value}
            // or {"DB": {...}}. Guard on the literal substring so those
            // frames skip the typed probe entirely (no TypeProbeJson
            // allocation, no glz::format_error on a parse we know will fail).
            // Only frames that advertise "type" pay the probe, and only
            // ack/dictionary pay the full typed read — and both use
            // read<T>(string_view) on the raw wire bytes, not the old
            // Value + readFromValue() double walk.
            if (t_msg->str.find("\"type\"") != std::string::npos) {
                auto probe = sgrn::json::read<TypeProbeJson>(t_msg->str);
                if (!probe.hasError() && !probe.value().type.empty()) {
                    const std::string& msg_type = probe.value().type;
                    if (msg_type == "dictionary") {
                        onDictionaryFrame(t_msg->str);
                        return;
                    }
                    if (msg_type == "write_ack") {
                        auto w_res = sgrn::json::read<WriteAckJson>(t_msg->str);
                        if (!w_res.hasError() && w_res.value().sequence.has_value()) {
                            std::lock_guard<std::mutex> lk(ack_mutex_);
                            last_ack_sequence_ = w_res.value().sequence.value();
                            last_ack_ok_ = w_res.value().ok.value_or(false);
                            ack_cv_.notify_all();
                        }
                        return;
                    }
                }
            }
            handleDeltaSnapshot(t_msg->str, 0);
            break;
        default:
            break;
    }
}

bool GatewaySync::writeFieldThroughRuntime(const std::string& t_target, const std::string& t_raw_val, std::string& t_err) {
    if (!runtime_) {
        t_err = "runtime is null";
        return false;
    }

    auto ft = runtime_->getSchema().parseFieldTarget(t_target);
    if (!ft) {
        // TIA-style discrete tag (flat-delta leaf resolved through the
        // dictionary): tags live outside the DB filter model and always
        // sync. Tags are discrete areas outside the DB filter model and
        // always sync.
        if (runtime_->hasTag(t_target)) {
            const std::string json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
            // Gateway-originated control writes must not echo back uplink —
            // same suppression as the DB path (thread-local, same thread).
            suppress_publish_ = true;
            auto wres = runtime_->writeTagJson(t_target, json_val);
            suppress_publish_ = false;
            if (wres.hasError()) {
                t_err = wres.error();
                return false;
            }
            return true;
        }
        t_err = fmt::format("symbolic path '{}' not found in schema", t_target);
        return false;
    }
    if (!isDbSubscribed(ft->db_number))
        return true;

    const std::string json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    auto write_res = runtime_->getMemory().updateField(ft->db_number, ft->field_path, json_val);
    if (write_res.hasError()) {
        t_err = toString(write_res.error());
        return false;
    }
    runtime_->getMemory().processor()->processCommands();

    if (auto db_res = runtime_->getSchema().getDb(ft->db_number); !db_res.hasError()) {
        // Gateway-originated deltas are already reflected in Gateway memory.
        // Mark the local runtime dirty for other shell endpoints, but suppress
        // the outbound observer so the same delta is not echoed back over HTTP.
        suppress_publish_ = true;
        runtime_->markDirty(ft->db_number, 0, static_cast<uint32_t>(db_res.value()->size_bytes));
        suppress_publish_ = false;
    }
    return true;
}

void GatewaySync::onDictionaryFrame(const std::string& t_json_payload) {
    // Direct typed read on the raw wire bytes: no glz::generic_i64
    // intermediate, one reflection walk instead of parse + re-serialize +
    // re-parse. Same wire shape, meaningfully less work per connect.
    auto frame_res = sgrn::json::read<DictionaryFrameJson>(t_json_payload);
    if (frame_res.hasError())
        return;
    const auto& frame = frame_res.value();
    std::vector<std::string> paths;
    for (const auto& item : frame.leaves) {
        if (item.id >= paths.size())
            paths.resize(static_cast<size_t>(item.id) + 1);
        paths[item.id] = item.path;
    }
    {
        std::lock_guard<std::mutex> lk(dict_mutex_);
        remote_paths_ = std::move(paths);
        has_remote_dict_ = true;
    }
    fmt::print(fg(fmt::color::cyan), "[GatewayBinding] Leaf dictionary: {} paths.\n", frame.leaves.size());
}

bool GatewaySync::resolveLeafPath(uint32_t t_id, std::string& t_path) {
    std::lock_guard<std::mutex> lk(dict_mutex_);
    // Authoritative first: the server's own decode table.
    if (has_remote_dict_ && t_id < remote_paths_.size() && !remote_paths_[t_id].empty()) {
        t_path = remote_paths_[t_id];
        return true;
    }
    // Fallback: locally built table (same buildFrom() order as any gateway
    // serving this schema, so IDs agree when schemas match).
    if (local_dict_built_ && t_id < local_dict_.path_by_id.size()) {
        t_path = local_dict_.path_by_id[t_id];
        return true;
    }
    return false;
}

void GatewaySync::handleDeltaSnapshot(const std::string& t_json_payload, uint16_t /*db_hint*/) {
    // Delta payloads have arbitrary keys ({"<leaf-id>": value} or
    // {"DB": {...}}), so they must stay dynamic. Clear the existing object
    // rather than replacing it, allowing Glaze's parser to reuse its storage.
    //
    // Single-thread ownership: handleDeltaSnapshot() is called only from
    // onMessage(), which IXWebSocket invokes on one receive thread per
    // connection. No public method touches delta_value_ / leaf_scratch_, so
    // both remain callback-thread-only. The publish worker has its own
    // json_scratch_ buffer.
    if (delta_value_.is_object())
        delta_value_.get_object().clear();
    else
        delta_value_ = glz::generic_i64::object_t{};
    if (auto r = sgrn::json::readInto(delta_value_, t_json_payload); r.hasError()) {
        fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Unparseable message: {}\n", t_json_payload.substr(0, 80));
        return;
    }
    if (!delta_value_.is_object()) {
        fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Unparseable message: {}\n", t_json_payload.substr(0, 80));
        return;
    }

    for (const auto& [name, val] : delta_value_.get_object()) {
        // Flat leaf-id delta (dictionary mode): {"<id>": value}. The value
        // is a scalar for scalar tags but an OBJECT for UDT tags — so the
        // numeric key decides, not the value shape. Unresolvable numeric
        // keys with object values fall through to the nested DB path (a DB
        // literally named "123" still works).
        uint32_t leaf_id = 0;
        std::string leaf_path;
        const char* first = name.data();
        const char* last = first + name.size();
        const auto [parsed_end, parse_error] = std::from_chars(first, last, leaf_id);
        const bool is_leaf_id = parse_error == std::errc{} && parsed_end == last && resolveLeafPath(leaf_id, leaf_path);

        if (is_leaf_id) {
            if (auto result = sgrn::json::writeTo(leaf_scratch_, val); result.hasError()) {
                fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Could not serialize leaf '{}': {}\n", leaf_path,
                    result.error().message);
                continue;
            }
            std::string t_err;
            if (!writeFieldThroughRuntime(leaf_path, leaf_scratch_, t_err))
                fmt::print(stderr, fg(fmt::color::red), "[GatewayBinding] Runtime write failed for '{}': {}\n", leaf_path, t_err);
            continue;
        }
        if (!val.is_object())
            continue;

        std::string path = name;
        auto apply_leaf = [this](const std::string& t_path, const sgrn::json::Value& t_value) {
            if (auto result = sgrn::json::writeTo(leaf_scratch_, t_value); result.hasError()) {
                fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Could not serialize delta value at '{}': {}\n", t_path,
                    result.error().message);
                return;
            }
            std::string t_err;
            if (!writeFieldThroughRuntime(t_path, leaf_scratch_, t_err))
                fmt::print(stderr, fg(fmt::color::red), "[GatewayBinding] Runtime write failed for '{}': {}\n", t_path, t_err);
        };
        ::sgrn::gateway::common::visitJsonLeaves(val, path, '.', ::sgrn::gateway::common::JsonArrayPolicy::Skip, apply_leaf);
    }
}

void GatewaySync::onRuntimeDirty(uint16_t t_db, uint32_t, uint32_t) {
    if (suppress_publish_ || !publish_on_dirty_ || !connected_ || !isDbSubscribed(t_db))
        return;
    requestPublish();
}

void GatewaySync::onTagDirty(const std::string& t_name) {
    // Tags live outside the DB subscription model and always publish (when
    // enabled) — discrete control values must reach the gateway.
    if (suppress_publish_ || !publish_on_dirty_ || !connected_)
        return;
    requestPublish();
}

void GatewaySync::requestPublish() {
    if (!runtime_ || !publish_on_dirty_ || !connected_)
        return;
    {
        std::lock_guard<std::mutex> lk(publish_mutex_);
        publish_requested_ = true;
    }
    publish_cv_.notify_one();
}

void GatewaySync::publishWorkerLoop() {
    std::unique_lock<std::mutex> lk(publish_mutex_);
    while (!stop_publish_worker_) {
        publish_cv_.wait(lk, [this]() { return publish_requested_ || stop_publish_worker_; });
        if (stop_publish_worker_)
            break;

        // Coalesce scan-cycle bursts before touching the dirty ledger. This
        // keeps a group of per-field marks from becoming per-field frames.
        publish_requested_ = false;
        publish_cv_.wait_for(lk, std::chrono::milliseconds(publish_coalesce_ms_), [this]() { return stop_publish_worker_; });
        if (stop_publish_worker_)
            break;

        lk.unlock();
        (void)publishDirtyBatch();
        lk.lock();

        // A writer may have dirtied another region while publishDirtyBatch()
        // was reading memory or waiting on HTTP. Loop again without sleeping
        // forever behind a stale false flag.
        for (const auto& db_entry : runtime_->getSchema().dbs()) {
            const auto db_num = db_entry.first;
            if (isDbSubscribed(db_num) && runtime_->hasDirty(db_num)) {
                publish_requested_ = true;
                break;
            }
        }
        if (!publish_requested_ && !runtime_->peekPublishTags().empty())
            publish_requested_ = true;
    }
}

bool GatewaySync::publishDirtyBatch() {
    if (!runtime_)
        return false;

    bool expected = false;
    if (!publishing_.compare_exchange_strong(expected, true))
        return true;

    std::vector<SyncBatchItemJson> updates;
    std::vector<std::pair<uint16_t, ::sgrn::plcsim::runtime::DirtyRegion>> attempted_regions;
    std::vector<std::pair<uint16_t, ::sgrn::plcsim::runtime::DirtyRegion>> unsent_regions;
    std::vector<::sgrn::gateway::adapters::websocket::runtime_sync::Record> sync_records;

    for (const auto& db_entry : runtime_->getSchema().dbs()) {
        const auto db_num = db_entry.first;
        if (!isDbSubscribed(db_num) || !runtime_->hasDirty(db_num))
            continue;
        auto regions = runtime_->takeDirty(db_num);
        for (const auto& region : regions) {
            std::vector<uint8_t> bytes(region.length, 0);
            if (auto r = runtime_->getMemory().readDbMemory(db_num, region.offset, region.length, bytes.data()); !r) {
                std::lock_guard<std::mutex> lk(err_mutex_);
                last_error_ = std::string(toString(r.error()));
                unsent_regions.emplace_back(db_num, region);
                continue;
            }
            SyncBatchItemJson item;
            item.db = db_num;
            item.offset = region.offset;
            item.size = region.length;
            item.data = sgrn::utils::encoding::toBase64Url(bytes.data(), bytes.size());
            updates.push_back(std::move(item));
            sync_records.push_back({db_num, region.offset, std::move(bytes)});
            attempted_regions.emplace_back(db_num, region);
        }
    }

    bool db_ok = true;
    if (updates.empty()) {
        // readDbMemory() failed for every region. Put them back into the
        // canonical dirty ledger without waking this same publisher again.
        suppress_publish_ = true;
        for (const auto& [db_num, region] : unsent_regions)
            runtime_->markDirty(db_num, region.offset, region.length);
        suppress_publish_ = false;
        db_ok = unsent_regions.empty();
    } else {
        bool sent = false;
        std::string send_error;
        const uint64_t sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        if (binary_transport_) {
            using namespace ::sgrn::gateway::adapters::websocket::runtime_sync;
            Frame frame;
            frame.kind = Kind::Write;
            frame.sequence = sequence;
            frame.timestamp_ms = static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());
            frame.records = std::move(sync_records);
            const std::string payload = encode(frame);
            auto result = ws_.sendBinary(payload);
            sent = !payload.empty() && result.success;
            if (!sent)
                send_error = "RuntimeSync binary WebSocket send failed";
        } else {
            SyncBatchEnvelopeJson envelope;
            envelope.command = "write";
            envelope.sequence = sequence;
            envelope.updates = std::move(updates);
            // writeTo reuses json_scratch_ across frames (avoids one
            // std::string allocation per publish on the worker hot path).
            if (auto w = sgrn::json::writeTo(json_scratch_, envelope); w.hasError()) {
                send_error = "RuntimeSync JSON envelope serialization failed";
            } else {
                auto result = ws_.sendText(json_scratch_);
                sent = result.success;
                if (!sent)
                    send_error = "RuntimeSync JSON WebSocket send failed";
            }
        }
        bool acknowledgement_timed_out = false;
        if (sent) {
            std::unique_lock<std::mutex> lk(ack_mutex_);
            const bool acknowledged =
                ack_cv_.wait_for(lk, std::chrono::seconds(5), [this, sequence]() { return last_ack_sequence_ == sequence; });
            if (!acknowledged || !last_ack_ok_) {
                sent = false;
                acknowledgement_timed_out = !acknowledged;
                send_error = acknowledged ? "RuntimeSync write rejected by gateway" : "RuntimeSync write acknowledgement timed out";
            }
        }

        // Older gateways accept the original HTTP batch API but do not know the
        // RuntimeSync WebSocket write command. Preserve interoperability without
        // paying the HTTP round-trip on the normal acknowledged path.
        if (!sent && acknowledgement_timed_out && !http_base_url_.empty()) {
            // Order safety: the WS batch above carried memory as of take-time
            // (up to ~5 s old after an ack timeout). Pushing those stale bytes
            // through the second (HTTP) channel could apply them AFTER newer
            // batches already acked over WS, regressing the gateway twin.
            // Re-read the same regions fresh: the fallback then carries the
            // newest known state, which is always safe to apply last. Old
            // gateways without WS write support still converge over their only
            // channel. On re-read failure, keep the batch for retry instead of
            // pushing possibly-torn state.
            std::vector<SyncBatchItemJson> fresh_updates;
            bool fresh_ok = !attempted_regions.empty();
            for (const auto& [db_num, region] : attempted_regions) {
                std::vector<uint8_t> fresh_bytes(region.length, 0);
                if (auto r = runtime_->getMemory().readDbMemory(db_num, region.offset, region.length, fresh_bytes.data()); !r) {
                    fresh_ok = false;
                    break;
                }
                SyncBatchItemJson item;
                item.db = db_num;
                item.offset = region.offset;
                item.size = region.length;
                item.data = sgrn::utils::encoding::toBase64Url(fresh_bytes.data(), fresh_bytes.size());
                fresh_updates.push_back(std::move(item));
            }
            if (!fresh_ok || fresh_updates.empty()) {
                send_error = "RuntimeSync fallback re-read failed; batch kept for retry";
            } else {
                // Rare path (5 s ack timeout) — a fresh string here is fine;
                // do NOT reuse json_scratch_ (different shape: bare array).
                auto fresh_body_res = sgrn::json::write(fresh_updates);
                if (fresh_body_res.hasError()) {
                    send_error = "RuntimeSync fallback serialization failed";
                } else {
                    httplib::Client client(http_base_url_);
                    client.set_connection_timeout(2, 0);
                    client.set_read_timeout(5, 0);
                    client.set_write_timeout(5, 0);
                    auto response = client.Put("/memory/batch", fresh_body_res.value(), "application/json");
                    if (response && response->status >= 200 && response->status < 300) {
                        sent = true;
                    } else {
                        send_error = response ? fmt::format("RuntimeSync fallback failed: HTTP {} {}", response->status, response->body)
                                              : "RuntimeSync fallback failed: no HTTP response";
                    }
                }
            }
        }

        if (!sent) {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = send_error;
            unsent_regions.insert(unsent_regions.end(), attempted_regions.begin(), attempted_regions.end());
        }

        if (!unsent_regions.empty()) {
            // Failed transport writes or local read errors must not acknowledge dirty
            // regions. Re-mark through PlcRuntime so later publishes retry the
            // same canonical dirty state instead of maintaining a second queue.
            suppress_publish_ = true;
            for (const auto& [db_num, region] : unsent_regions)
                runtime_->markDirty(db_num, region.offset, region.length);
            suppress_publish_ = false;
        }

        db_ok = sent && unsent_regions.empty();
    }

    // Discrete tags ride the reliable-uplink ledger (take + restore on
    // failure), independent of the broadcast ledger gateways consume.
    const bool tag_ok = publishPendingTags();
    publishing_ = false;
    return db_ok && tag_ok;
}

namespace
{
// Percent-encode a tag name for the HTTP POST /tags/<name> fallback.
std::string percentEncodeTagName(const std::string& t_in) {
    std::string out;
    out.reserve(t_in.size());
    for (char c : t_in) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", uc);
            out += buf;
        }
    }
    return out;
}
} // namespace

bool GatewaySync::publishPendingTags() {
    if (!runtime_)
        return false;

    // Take from the reliable ledger (broadcasts consume the other one).
    const std::vector<std::string> attempted = runtime_->takePublishTags();
    if (attempted.empty())
        return true;

    std::vector<SyncAreaItemJson> updates;
    std::vector<std::string> sent_names;
    std::vector<std::string> unsent;

    for (const auto& name : attempted) {
        auto desc = runtime_->describeTag(name);
        if (desc.hasError())
            continue; // tag vanished mid-flight; nothing to restore
        const auto& tag = desc.value();
        const size_t span = static_cast<size_t>(tag.span_bytes);
        std::vector<uint8_t> bytes(span, 0);
        if (auto r = runtime_->readAreaMemory(tag.addr.area, static_cast<size_t>(tag.addr.byte_offset), span, bytes.data()); r.hasError()) {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = r.error();
            unsent.push_back(name);
            continue;
        }
        SyncAreaItemJson item;
        item.area = static_cast<uint32_t>(tag.addr.area);
        item.offset = static_cast<uint64_t>(tag.addr.byte_offset);
        item.size = static_cast<uint64_t>(span);
        item.data = sgrn::utils::encoding::toBase64Url(bytes.data(), bytes.size());
        updates.push_back(std::move(item));
        sent_names.push_back(name);
    }

    bool sent = false;
    std::string send_error;
    bool acknowledgement_timed_out = false;
    if (!updates.empty()) {
        const uint64_t sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        // Areas always travel as JSON (control traffic is low-rate and stays
        // inspectable), even in binary transport mode.
        SyncAreaEnvelopeJson envelope;
        envelope.command = "write_area";
        envelope.sequence = sequence;
        envelope.updates = std::move(updates);
        // Same scratch buffer as the DB envelope: publishPendingTags() runs
        // after publishDirtyBatch() on the single worker thread, and
        // sendText copies into IXWebSocket's frame buffer before returning.
        if (auto w = sgrn::json::writeTo(json_scratch_, envelope); w.hasError()) {
            send_error = "RuntimeSync area envelope serialization failed";
        } else {
            auto result = ws_.sendText(json_scratch_);
            sent = result.success;
            if (!sent) {
                send_error = "RuntimeSync area WebSocket send failed";
            } else {
                std::unique_lock<std::mutex> lk(ack_mutex_);
                const bool acknowledged =
                    ack_cv_.wait_for(lk, std::chrono::seconds(5), [this, sequence]() { return last_ack_sequence_ == sequence; });
                if (!acknowledged || !last_ack_ok_) {
                    sent = false;
                    acknowledgement_timed_out = !acknowledged;
                    send_error = acknowledged ? "RuntimeSync area write rejected by gateway" : "RuntimeSync area write timed out";
                }
            }
        }
    } else {
        sent = unsent.empty();
        if (!sent && send_error.empty())
            send_error = "RuntimeSync area publish: tag reads failed";
    }

    // Gateways without area support (or pre-/tags HTTP) neither ack nor 404
    // cleanly on the fast path — fall back to per-tag HTTP POST, which also
    // serves future tag-capable HTTP endpoints.
    if (!sent && acknowledgement_timed_out && !http_base_url_.empty()) {
        httplib::Client client(http_base_url_);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        client.set_write_timeout(5, 0);
        sent = !sent_names.empty();
        for (const auto& name : sent_names) {
            auto jr = runtime_->readTagJson(name);
            if (jr.hasError()) {
                sent = false;
                send_error = "RuntimeSync area fallback: tag read failed";
                break;
            }
            auto response = client.Post(("/tags/" + percentEncodeTagName(name)).c_str(), jr.value(), "application/json");
            if (!response || response->status < 200 || response->status >= 300) {
                sent = false;
                send_error = response ? fmt::format("RuntimeSync area fallback failed: HTTP {} {}", response->status, response->body)
                                      : "RuntimeSync area fallback failed: no HTTP response";
                break;
            }
        }
    }

    if (!sent) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = send_error;
        unsent.insert(unsent.end(), sent_names.begin(), sent_names.end());
    }
    if (!unsent.empty())
        runtime_->restorePublishTags(unsent);
    return sent && unsent.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// GatewayServer Implementation
// ─────────────────────────────────────────────────────────────────────────────
GatewayServer::GatewayServer(PlcRuntimeSPtr tsp_runtime)
    : runtime_(std::move(tsp_runtime)) {
}

GatewayServer::~GatewayServer() {
    stop();
}

bool GatewayServer::start(uint16_t t_port) {
    port_ = t_port;
    running_.store(true);
    return true;
}

void GatewayServer::stop() {
    running_.store(false);
}

void GatewayServer::broadcast() {
    if (runtime_) {
        // Broadcast flushes dirty state from runtime
    }
}

std::string GatewayServer::getLastError() const {
    std::lock_guard<std::mutex> lk(err_mutex_);
    return last_error_;
}

} // namespace sgrn::s7shell::connection
