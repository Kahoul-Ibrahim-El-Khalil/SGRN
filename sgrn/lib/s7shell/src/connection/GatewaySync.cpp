#include <sgrn/s7shell/connection/GatewaySync.hpp>

#include <sgrn/gateway/adapters/websocket/RuntimeSyncProtocol.hpp>
#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::connection
{
thread_local bool GatewaySync::suppress_publish_ = false;

static std::string rapidjsonValueToString(const rapidjson::Value& t_v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    t_v.Accept(writer);
    return sb.GetString();
}

static std::string jsonString(const std::string& t_value) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.String(t_value.c_str(), static_cast<rapidjson::SizeType>(t_value.size()));
    return sb.GetString();
}

static std::string websocketUrlToHttpBase(std::string t_url) {
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

static void flattenJson(
    const rapidjson::Value& t_node, const std::string& t_prefix, std::vector<std::pair<std::string, std::string>>& t_out) {
    if (t_node.IsObject()) {
        for (auto it = t_node.MemberBegin(); it != t_node.MemberEnd(); ++it) {
            std::string child_key = t_prefix.empty() ? it->name.GetString() : t_prefix + "." + it->name.GetString();
            flattenJson(it->value, child_key, t_out);
        }
        return;
    }
    if (!t_node.IsArray())
        t_out.emplace_back(t_prefix, rapidjsonValueToString(t_node));
}

GatewaySync::GatewaySync(PlcRuntimeSPtr tsp_runtime)
    : runtime_(std::move(tsp_runtime)) {
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
    {
        std::lock_guard<std::mutex> lk(subs_mutex_);
        subscribed_dbs_.erase(t_db);
    }
    if (connected_ && runtime_) {
        auto schema = runtime_->getSchema().getDb(t_db);
        if (!schema.hasError() && schema.value()) {
            const std::string command = R"({"command":"unsubscribe","path":)" + jsonString(schema.value()->db_name) + "}";
            (void)ws_.sendText(command);
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
                {
                    std::lock_guard<std::mutex> lk(subs_mutex_);
                    selected = subscribed_dbs_;
                }
                for (const auto& [db_num, db_schema] : runtime_->getSchema().dbs()) {
                    if (!selected.empty() && !selected.contains(db_num))
                        continue;
                    const std::string command = R"({"command":"subscribe","path":)" + jsonString(db_schema.db_name) + "}";
                    (void)ws_.sendText(command);
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
            {
                rapidjson::Document ack_doc;
                ack_doc.Parse(t_msg->str.c_str());
                if (!ack_doc.HasParseError() && ack_doc.IsObject() && ack_doc.HasMember("type") && ack_doc["type"].IsString()) {
                    const std::string msg_type = ack_doc["type"].GetString();
                    // Leaf-dictionary decode table for flat deltas. The
                    // server pushes this once right after the full-snapshot
                    // seed on every connect (see WebSocketAdapter onopen).
                    if (msg_type == "dictionary") {
                        onDictionaryFrame(t_msg->str);
                        return;
                    }
                    if (msg_type == "write_ack") {
                        if (ack_doc.HasMember("sequence") && ack_doc["sequence"].IsUint64()) {
                            std::lock_guard<std::mutex> lk(ack_mutex_);
                            last_ack_sequence_ = ack_doc["sequence"].GetUint64();
                            last_ack_ok_ = ack_doc.HasMember("ok") && ack_doc["ok"].IsBool() && ack_doc["ok"].GetBool();
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
    rapidjson::Document doc;
    doc.Parse(t_json_payload.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("leaves") || !doc["leaves"].IsArray())
        return;
    std::vector<std::string> paths;
    for (const auto& item : doc["leaves"].GetArray()) {
        if (!item.IsObject() || !item.HasMember("id") || !item["id"].IsUint() || !item.HasMember("path") || !item["path"].IsString())
            continue;
        const uint32_t id = item["id"].GetUint();
        if (id >= paths.size())
            paths.resize(static_cast<size_t>(id) + 1);
        paths[id] = item["path"].GetString();
    }
    {
        std::lock_guard<std::mutex> lk(dict_mutex_);
        remote_paths_ = std::move(paths);
        has_remote_dict_ = true;
    }
    fmt::print(fg(fmt::color::cyan), "[GatewayBinding] Leaf dictionary: {} paths.\n", doc["leaves"].Size());
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
    rapidjson::Document doc;
    doc.Parse(t_json_payload.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Unparseable message: {}\n", t_json_payload.substr(0, 80));
        return;
    }

    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        // Flat leaf-id delta (dictionary mode): {"<id>": value}. The value
        // is a scalar for scalar tags but an OBJECT for UDT tags — so the
        // numeric key decides, not the value shape. Unresolvable numeric
        // keys with object values fall through to the nested DB path (a DB
        // literally named "123" still works).
        bool is_leaf_id = false;
        std::string leaf_path;
        try {
            size_t pos = 0;
            const unsigned long parsed = std::stoul(it->name.GetString(), &pos);
            if (it->name.GetString()[pos] == '\0' && resolveLeafPath(static_cast<uint32_t>(parsed), leaf_path))
                is_leaf_id = true;
        } catch (const std::exception&) {
        }
        if (is_leaf_id) {
            std::string t_err;
            if (!writeFieldThroughRuntime(leaf_path, rapidjsonValueToString(it->value), t_err))
                fmt::print(stderr, fg(fmt::color::red), "[GatewayBinding] Runtime write failed for '{}': {}\n", leaf_path, t_err);
            continue;
        }
        if (!it->value.IsObject())
            continue;

        const std::string db_root = it->name.GetString();
        std::vector<std::pair<std::string, std::string>> fields;
        flattenJson(it->value, db_root, fields);

        for (const auto& [path, value] : fields) {
            std::string t_err;
            if (!writeFieldThroughRuntime(path, value, t_err))
                fmt::print(stderr, fg(fmt::color::red), "[GatewayBinding] Runtime write failed for '{}': {}\n", path, t_err);
        }
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
        publish_cv_.wait_for(lk, std::chrono::milliseconds(25), [this]() { return stop_publish_worker_; });
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

    rapidjson::Document doc;
    doc.SetArray();
    auto& alloc = doc.GetAllocator();

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
            rapidjson::Value item(rapidjson::kObjectType);
            item.AddMember("db", db_num, alloc);
            item.AddMember("offset", region.offset, alloc);
            item.AddMember("size", region.length, alloc);
            std::string data = sgrn::utils::encoding::toBase64Url(bytes.data(), bytes.size());
            item.AddMember("data", rapidjson::Value(data.c_str(), alloc), alloc);
            doc.PushBack(item, alloc);
            sync_records.push_back({db_num, region.offset, std::move(bytes)});
            attempted_regions.emplace_back(db_num, region);
        }
    }

    bool db_ok = true;
    if (doc.Empty()) {
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
        const std::string body = sgrn::utils::json::serializeCompact(doc);
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
            const std::string payload =
                std::string(R"({"command":"write","sequence":)") + std::to_string(sequence) + R"(,"updates":)" + body + "}";
            auto result = ws_.sendText(payload);
            sent = result.success;
            if (!sent)
                send_error = "RuntimeSync JSON WebSocket send failed";
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
            httplib::Client client(http_base_url_);
            client.set_connection_timeout(2, 0);
            client.set_read_timeout(5, 0);
            client.set_write_timeout(5, 0);
            auto response = client.Put("/memory/batch", body, "application/json");
            if (response && response->status >= 200 && response->status < 300) {
                sent = true;
            } else {
                send_error = response ? fmt::format("RuntimeSync fallback failed: HTTP {} {}", response->status, response->body)
                                      : "RuntimeSync fallback failed: no HTTP response";
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

    rapidjson::Document doc;
    doc.SetArray();
    auto& alloc = doc.GetAllocator();
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
        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("area", static_cast<unsigned>(tag.addr.area), alloc);
        item.AddMember("offset", static_cast<uint64_t>(tag.addr.byte_offset), alloc);
        item.AddMember("size", static_cast<uint64_t>(span), alloc);
        std::string data = sgrn::utils::encoding::toBase64Url(bytes.data(), bytes.size());
        item.AddMember("data", rapidjson::Value(data.c_str(), alloc), alloc);
        doc.PushBack(item, alloc);
        sent_names.push_back(name);
    }

    bool sent = false;
    std::string send_error;
    bool acknowledgement_timed_out = false;
    if (!doc.Empty()) {
        const std::string body = sgrn::utils::json::serializeCompact(doc);
        const uint64_t sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        // Areas always travel as JSON (control traffic is low-rate and stays
        // inspectable), even in binary transport mode.
        const std::string payload =
            std::string(R"({"command":"write_area","sequence":)") + std::to_string(sequence) + R"(,"updates":)" + body + "}";
        auto result = ws_.sendText(payload);
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
