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

} // namespace sgrn::gateway::adapters::websocket
