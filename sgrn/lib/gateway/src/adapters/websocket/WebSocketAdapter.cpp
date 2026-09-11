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

WebSocketAdapter::WebSocketAdapter() = default;
WebSocketAdapter::~WebSocketAdapter() {
    stop();
}

void WebSocketAdapter::configure(SecurityManagerSptr tsp_security_manager, const PlcSchemaStore* tp_registry,
    std::function<std::string()> t_full_snapshot_provider, BinaryReadFn t_binary_read_fn) {
    security_manager_ = std::move(tsp_security_manager);
    registry_ = tp_registry;
    full_snapshot_provider_ = std::move(t_full_snapshot_provider);
    binary_read_fn_ = std::move(t_binary_read_fn);
}

sgrn::Result<void> WebSocketAdapter::start(const std::string& t_ip, uint16_t t_port, SecurityManagerSptr tsp_security_manager,
    const PlcSchemaStore* tp_registry, std::function<std::string()> t_full_snapshot_provider, BinaryReadFn t_binary_read_fn) {
    if (running_.load(std::memory_order_acquire))
        return sgrn::Result<void>::Error("WebSocketAdapter: already running");
    configure(std::move(tsp_security_manager), tp_registry, std::move(t_full_snapshot_provider), std::move(t_binary_read_fn));

    server_ = std::make_unique<northbound::NorthboundServer>();
    registerRoutes(server_->app());
    if (auto r = server_->start(t_ip, t_port); r.hasError()) {
        server_.reset();
        return fmt::format("WebSocket server failed to listen: {}", r.error());
    }
    running_.store(true, std::memory_order_release);

    broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_event) { handleTelemetryEvent(t_event); });
    return {};
}

void WebSocketAdapter::registerRoutes(crow::SimpleApp& t_app) {
    // Subscribe to the broker on first registration so telemetry flows even
    // on the unified path (which never calls start()). Idempotent.
    if (broker_sub_id_ == 0) {
        broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_event) { handleTelemetryEvent(t_event); });
    }

    struct HandshakeInfo {
        std::string ip;
        std::string origin;
        std::vector<std::string> headers;
    };

    CROW_WEBSOCKET_ROUTE(t_app, "/ws")
        .onaccept([this](const crow::request& t_req, std::optional<crow::response>& t_res, void** tp_userdata) {
            std::vector<std::string> header_names;
            std::string origin;
            for (const auto& [k, v] : t_req.headers) {
                header_names.push_back(k);
                if (k == "Origin" || k == "origin")
                    origin = v;
            }
            if (security_manager_) {
                // Common SecurityHelper for connection authorization.
                // Rejecting here fails the handshake instead of closing
                // right after it (the old post-Open close()).
                auto auth = SecurityHelper::authorizeConnection(
                    *security_manager_, security::Protocol::WebSocket, t_req.remote_ip_address, std::nullopt, origin);
                if (auth.hasError()) {
                    SGRN_WARN_LOG("WebSocket connection from {} denied", t_req.remote_ip_address);
                    t_res = crow::response(403);
                    return;
                }
            }
            // Stash handshake context for onopen()/per-message auth. Freed in
            // onclose(); Crow never touches it. (If the TCP connection dies
            // between accept and open, one small struct leaks — negligible
            // and unavoidable without a pre-open teardown hook.)
            *tp_userdata = new HandshakeInfo{t_req.remote_ip_address, std::move(origin), std::move(header_names)};
        })
        .onopen([this](crow::websocket::connection& t_conn) {
            auto* p_info = static_cast<HandshakeInfo*>(t_conn.userdata());
            std::string ip = p_info ? p_info->ip : t_conn.get_remote_ip();
            std::string origin = p_info ? p_info->origin : "";
            std::vector<std::string> headers = p_info ? p_info->headers : std::vector<std::string>{};

            // The client is NOT registered in clients_ until the seed frames
            // below are queued, guaranteeing that the delta stream
            // (TelemetryBroker) never reaches the client before its initial
            // state frame.

            // Seed the freshly-connected client with the current full plant
            // state before any DeltaSnapshot can be forwarded. After a
            // gateway restart this is the persisted ("reclaimed") twin, so
            // a viewer never sees an empty process image until the PLC
            // happens to push a delta.
            //
            // NOTE: sends happen before registration on purpose (wire order:
            // full snapshot → dictionary → deltas). The sends post into
            // Crow's io_context; registration right after pins the order
            // because both run through the same per-connection write queue.
            if (full_snapshot_provider_) {
                std::string initial = full_snapshot_provider_();
                if (!initial.empty() && initial != "{}")
                    t_conn.send_text(initial);
            }

            // Push the leaf dictionary once at connect time so clients
            // that opt into dictionary mode can decode flat id-keyed
            // payloads before the first batch arrives.
            if (dict_ && !dict_->path_by_id.empty()) {
                rapidjson::StringBuffer dsb;
                rapidjson::Writer<rapidjson::StringBuffer> dw(dsb);
                dw.StartObject();
                dw.Key("type");
                dw.String("dictionary");
                dw.Key("leaves");
                dw.StartArray();
                for (size_t id = 0; id < dict_->path_by_id.size(); ++id) {
                    const auto& path = dict_->path_by_id[id];
                    dw.StartObject();
                    dw.Key("id");
                    dw.Uint(static_cast<uint32_t>(id));
                    dw.Key("path");
                    dw.String(path.c_str(), static_cast<rapidjson::SizeType>(path.size()));
                    dw.EndObject();
                }
                dw.EndArray();
                dw.EndObject();
                t_conn.send_text(dsb.GetString());
            }

            // The handshake request headers are stashed in userdata at accept
            // time (see onaccept above) — Crow does not retain them on the
            // connection itself.
            std::lock_guard<std::mutex> lk(clients_mutex_);
            clients_[&t_conn] = ClientContext{std::move(ip), std::move(origin), std::move(headers), {}, {}, false, {}};
        })
        .onmessage([this](crow::websocket::connection& t_conn, const std::string& t_data, bool t_is_binary) {
            if (t_is_binary)
                return; // text-command protocol only
            handleClientMessage(t_conn, t_data);
        })
        .onclose([this](crow::websocket::connection& t_conn, const std::string&, uint16_t) {
            delete static_cast<HandshakeInfo*>(t_conn.userdata());
            t_conn.userdata(nullptr);
            std::lock_guard<std::mutex> lk(clients_mutex_);
            clients_.erase(&t_conn);
        })
        .onerror([](crow::websocket::connection&, const std::string&) {
            // Transport-level error; the connection is torn down by Crow and
            // onclose() performs the bookkeeping.
        });
}

void WebSocketAdapter::stop() {
    if (broker_sub_id_ != 0) {
        TelemetryBroker::instance().unsubscribe(broker_sub_id_);
        broker_sub_id_ = 0;
    }
    if (server_)
        server_->stop();
    server_.reset();
    running_.store(false, std::memory_order_release);
    // NOTE: on the unified path there is no owned server_; the shared
    // listener is stopped by its owner (GatewayApplication). Client entries
    // belonging to it are dropped by Crow's own onclose during shutdown.
    std::lock_guard<std::mutex> lk(clients_mutex_);
    clients_.clear();
}

void WebSocketAdapter::sendText(crow::websocket::connection& t_conn, const std::string& t_payload) {
    try {
        t_conn.send_text(t_payload);
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("WebSocket send failed: {}", e.what());
    } catch (...) {
        SGRN_WARN_LOG("WebSocket send failed with unknown error");
    }
}

void WebSocketAdapter::sendBinary(crow::websocket::connection& t_conn, const void* tp_data, size_t t_size) {
    try {
        t_conn.send_binary(std::string(static_cast<const char*>(tp_data), t_size));
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("WebSocket binary send failed: {}", e.what());
    } catch (...) {
        SGRN_WARN_LOG("WebSocket binary send failed with unknown error");
    }
}

} // namespace sgrn::gateway::adapters::websocket
