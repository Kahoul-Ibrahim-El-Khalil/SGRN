#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/northbound/NorthboundServer.hpp>
#include <asio.hpp>
#include <chrono>
#include <thread>

namespace sgrn::gateway::adapters::northbound
{

namespace
{

/// Fixed worker count for the northbound asio pool. Dashboard/REST traffic is
/// light; the heavy PLC, persistence, and compression work already has its own
/// pools (GatewayApplication light/heavy/GlobalContext).
constexpr unsigned kCrowConcurrency = 4;

/// Probe that t_ip:t_port accepts TCP connections. Crow binds asynchronously
/// inside run_async() and reports bind failures only via its log, so an
/// explicit readiness probe is the only way to turn "port already in use"
/// into a proper Result error like the old httplib bind_to_port() check did.
/// A successful connect is immediately closed; Crow treats the bare probe
/// connection as a malformed request and drops it harmlessly.
bool waitUntilListening(const std::string& t_ip, uint16_t t_port) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            asio::io_context ctx;
            asio::ip::tcp::socket sock(ctx);
            asio::ip::tcp::endpoint ep(asio::ip::make_address(t_ip), t_port);
            std::error_code ec;
            sock.connect(ep, ec);
            if (!ec) {
                std::error_code ignored;
                sock.close(ignored);
                return true;
            }
        } catch (...) {
            // make_address threw (unresolvable bind address) — retry loop
            // covers transient states; a permanently bad address times out.
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

} // namespace

NorthboundServer::NorthboundServer() {
    app_.loglevel(crow::LogLevel::Warning);
    app_.concurrency(kCrowConcurrency);
}

NorthboundServer::~NorthboundServer() {
    stop();
}

sgrn::Result<void> NorthboundServer::start(const std::string& t_ip, uint16_t t_port) {
    if (running_.load(std::memory_order_acquire))
        return sgrn::Result<void>::Error("NorthboundServer: already running");

    app_.bindaddr(t_ip).port(t_port);
    // Validate routes synchronously: Crow otherwise reports route errors
    // (missing handler, bad parameter tag) as an exception inside the
    // run_async() future, where it would surface only as a bind timeout.
    try {
        app_.validate();
    } catch (const std::exception& e) {
        return fmt::format("NorthboundServer: invalid routes for {}:{}: {}", t_ip, t_port, e.what());
    }
    try {
        run_future_ = app_.run_async();
    } catch (const std::exception& e) {
        return fmt::format("NorthboundServer: failed to start on {}:{}: {}", t_ip, t_port, e.what());
    }

    if (!waitUntilListening(t_ip, t_port)) {
        std::string detail = "port in use?";
        // Harvest an early async failure (e.g. bind error thrown inside
        // run()) so it is reported instead of a generic timeout.
        if (run_future_.valid() && run_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                run_future_.get();
            } catch (const std::exception& e) {
                detail = e.what();
            } catch (...) {
                detail = "unknown startup error";
            }
        } else {
            try {
                app_.stop();
            } catch (...) {
            }
            if (run_future_.valid())
                run_future_.wait();
        }
        return fmt::format("NorthboundServer: failed to bind to {}:{} ({})", t_ip, t_port, detail);
    }

    running_.store(true, std::memory_order_release);
    return {};
}

void NorthboundServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;
    try {
        app_.stop();
    } catch (...) {
    }
    if (run_future_.valid())
        run_future_.wait();
}

} // namespace sgrn::gateway::adapters::northbound
