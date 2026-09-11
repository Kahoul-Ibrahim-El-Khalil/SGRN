#pragma once

// NorthboundServer — single asio-backed listener for the gateway's northbound
// traffic (HTTP REST + WebSocket).
//
// Replaces the previous split of cpp-httplib (HTTP, own thread pool) and
// IXWebSocket (WS, own event loop) with one Crow application:
//
//   * HTTP and WebSocket share the same asio io_context (Crow's internal one)
//     and the same TCP listener, so both can be served from a single port.
//     The WebSocket endpoint lives at `/ws` on the HTTP listener.
//   * All routes MUST be registered (via app()) BEFORE start() is called —
//     Crow resolves routes per request and registration is not thread-safe
//     once the server is running.
//
// Threading: Crow runs its own fixed-size asio thread pool (see
// NorthboundServer.cpp). Telemetry-driven WebSocket sends may be issued from
// any thread — crow::websocket::connection::send_* posts into the io_context
// and is thread-safe.

#include <sgrn/Result.hpp>
#include <atomic>
#include <crow.h>
#include <cstdint>
#include <future>
#include <string>

namespace sgrn::gateway::adapters::northbound
{

class NorthboundServer {
public:
    NorthboundServer();
    ~NorthboundServer();

    NorthboundServer(const NorthboundServer&) = delete;
    NorthboundServer& operator=(const NorthboundServer&) = delete;
    NorthboundServer(NorthboundServer&&) = delete;
    NorthboundServer& operator=(NorthboundServer&&) = delete;

    /// The Crow application. Register every HTTP route and the `/ws`
    /// WebSocket route here before calling start().
    crow::SimpleApp& app() {
        return app_;
    }

    /// Begin accepting on t_ip:t_port in the background. Verifies the port
    /// is actually listening before returning success.
    sgrn::Result<void> start(const std::string& t_ip, uint16_t t_port);

    /// Stop accepting and join the background loop. Idempotent.
    void stop();

    bool isRunning() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    crow::SimpleApp app_;
    std::future<void> run_future_;
    std::atomic<bool> running_{false};
};

} // namespace sgrn::gateway::adapters::northbound
