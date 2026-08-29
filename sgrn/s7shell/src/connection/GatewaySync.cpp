#include <sgrn/s7shell/connection/GatewaySync.hpp>

#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <chrono>
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::s7shell::connection
{
thread_local bool GatewaySync::suppress_publish_ = false;

static std::string rapidjsonValueToString(const rapidjson::Value& t_v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    t_v.Accept(writer);
    return sb.GetString();
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

GatewaySync::GatewaySync(PlcRuntimeSPtr tsp_runtime)
    : runtime_(std::move(tsp_runtime)) {
    if (runtime_) {
        dirty_observer_id_ =
            runtime_->addDirtyObserver([this](uint16_t t_db, uint32_t offset, uint32_t length) { onRuntimeDirty(t_db, offset, length); });
    }
    publish_worker_ = std::thread([this]() { publishWorkerLoop(); });
}

GatewaySync::~GatewaySync() {
    if (runtime_ && dirty_observer_id_ != 0)
        runtime_->removeDirtyObserver(dirty_observer_id_);
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
    std::lock_guard<std::mutex> lk(subs_mutex_);
    subscribed_dbs_.insert(t_db);
}

void GatewaySync::unsubscribeDb(uint16_t t_db) {
    std::lock_guard<std::mutex> lk(subs_mutex_);
    subscribed_dbs_.erase(t_db);
}

void GatewaySync::publishOnDirty(bool t_enabled) {
    publish_on_dirty_ = t_enabled;
    if (t_enabled)
        requestPublish();
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

void GatewaySync::handleDeltaSnapshot(const std::string& t_json_payload, uint16_t /*db_hint*/) {
    rapidjson::Document doc;
    doc.Parse(t_json_payload.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        fmt::print(stderr, fg(fmt::color::yellow), "[GatewayBinding] Unparseable message: {}\n", t_json_payload.substr(0, 80));
        return;
    }

    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
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
    if (suppress_publish_ || !publish_on_dirty_ || !connected_ || http_base_url_.empty() || !isDbSubscribed(t_db))
        return;
    requestPublish();
}

void GatewaySync::requestPublish() {
    if (!runtime_ || !publish_on_dirty_ || !connected_ || http_base_url_.empty())
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
        // keeps a group of per-field marks from becoming per-field HTTP calls.
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

    std::vector<std::pair<uint16_t, ::sgrn::s7shell::runtime::DirtyRegion>> attempted_regions;
    std::vector<std::pair<uint16_t, ::sgrn::s7shell::runtime::DirtyRegion>> unsent_regions;

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
            attempted_regions.emplace_back(db_num, region);
        }
    }

    if (doc.Empty()) {
        // readDbMemory() failed for every region. Put them back into the
        // canonical dirty ledger without waking this same publisher again.
        suppress_publish_ = true;
        for (const auto& [db_num, region] : unsent_regions)
            runtime_->markDirty(db_num, region.offset, region.length);
        suppress_publish_ = false;
        publishing_ = false;
        return unsent_regions.empty();
    }

    const std::string body = sgrn::utils::json::serializeCompact(doc);
    httplib::Client client(http_base_url_);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    auto res = client.Put("/memory/batch", body, "application/json");
    publishing_ = false;

    if (!res || res->status < 200 || res->status >= 300) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = res ? fmt::format("PUT /memory/batch failed: HTTP {} {}", res->status, res->body)
                          : "PUT /memory/batch failed: no HTTP response";
        unsent_regions.insert(unsent_regions.end(), attempted_regions.begin(), attempted_regions.end());
    }

    if (!unsent_regions.empty()) {
        // Failed HTTP writes or local read errors must not acknowledge dirty
        // regions. Re-mark through PlcRuntime so later publishes retry the
        // same canonical dirty state instead of maintaining a second queue.
        suppress_publish_ = true;
        for (const auto& [db_num, region] : unsent_regions)
            runtime_->markDirty(db_num, region.offset, region.length);
        suppress_publish_ = false;
    }

    if (!res || res->status < 200 || res->status >= 300)
        return false;

    return unsent_regions.empty();
}

} // namespace sgrn::s7shell::connection
