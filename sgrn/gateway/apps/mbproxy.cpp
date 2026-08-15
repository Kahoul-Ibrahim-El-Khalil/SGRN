#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/gateway/config/mbproxy.hpp>
#include <sgrn/gateway/wrappers/modbus/Client.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <asio.hpp>
#include <asio/thread_pool.hpp>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

using namespace sgrn::gateway::config;
using sgrn::gateway::wrappers::modbus::Client;
using sgrn::gateway::wrappers::modbus::ClientConfig;

// =============================================================================
// MbProxySession — Tracks a single Modbus mapping (Source Address -> Hub Address)
// =============================================================================
class MbProxySession : public std::enable_shared_from_this<MbProxySession> {
public:
    MbProxySession(asio::io_context& t_io, asio::thread_pool& t_pool, std::shared_ptr<Client> tsp_src, std::shared_ptr<Client> tsp_hub,
        const MbMapping& t_map)
        : timer_(t_io)
        , pool_(t_pool)
        , src_(std::move(tsp_src))
        , hub_(std::move(tsp_hub))
        , map_(t_map) {
        const int unit_size = (t_map.type == "coil") ? 1 : 2;
        buffer_.resize(t_map.count * unit_size, 0);
        last_state_.resize(t_map.count * unit_size, 0);
    }

    void start() {
        tick();
    }

private:
    void tick() {
        timer_.expires_after(std::chrono::milliseconds(map_.interval_ms));
        timer_.async_wait([self = shared_from_this()](std::error_code t_ec) {
            if (!t_ec)
                self->execute();
        });
    }

    void execute() {
        asio::post(pool_, [self = shared_from_this()] {
            self->do_poll_and_push();
            asio::post(self->timer_.get_executor(), [self] { self->tick(); });
        });
    }

    void do_poll_and_push() {
        if (src_->ensureConnected().hasError()) {
            fmt::print(stderr, fg(fmt::color::yellow), "[{}] Source Modbus connection offline.\n", plc_name_);
            return;
        }
        if (hub_->ensureConnected().hasError()) {
            fmt::print(stderr, fg(fmt::color::yellow), "[{}] Hub Modbus connection offline.\n", plc_name_);
            return;
        }

        bool read_ok = false;
        if (map_.type == "coil") {
            auto rc = src_->readCoils(map_.src_address, map_.count, buffer_.data());
            if (rc.hasError()) {
                src_->disconnect();
            } else {
                read_ok = true;
            }
        } else {
            auto rc = src_->readHoldingRegisters(map_.src_address, map_.count, reinterpret_cast<uint16_t*>(buffer_.data()));
            if (rc.hasError()) {
                src_->disconnect();
            } else {
                read_ok = true;
            }
        }

        if (!read_ok) {
            fmt::print(stderr, fg(fmt::color::red), "[{}] Poll Error ({} Addr: {} count: {})\n", plc_name_, map_.type, map_.src_address,
                map_.count);
            return;
        }

        const bool changed = first_run_ || (std::memcmp(buffer_.data(), last_state_.data(), buffer_.size()) != 0);
        if (!changed) {
            suppressed_count_++;
            if (verbose_ && suppressed_count_ % 100 == 0) {
                fmt::print("[{}] {} Addr: {} stable. Suppressed {} redundant writes.\n", plc_name_, map_.type, map_.src_address,
                    suppressed_count_);
            }
            return;
        }

        bool write_ok = false;
        if (map_.type == "coil") {
            auto rc = hub_->writeCoils(map_.dst_address, map_.count, buffer_.data());
            if (rc.hasError()) {
                hub_->disconnect();
            } else {
                write_ok = true;
            }
        } else {
            auto rc = hub_->writeRegisters(map_.dst_address, map_.count, reinterpret_cast<const uint16_t*>(buffer_.data()));
            if (rc.hasError()) {
                hub_->disconnect();
            } else {
                write_ok = true;
            }
        }

        if (!write_ok) {
            fmt::print(stderr, fg(fmt::color::red), "[{}] Push Error -> Hub ({} Addr: {})\n", plc_name_, map_.type, map_.dst_address);
        } else {
            std::memcpy(last_state_.data(), buffer_.data(), buffer_.size());
            first_run_ = false;
            suppressed_count_ = 0;
            if (verbose_) {
                fmt::print("[{}] Pushed {} Address {} -> Hub Address {}\n", plc_name_, map_.type, map_.src_address, map_.dst_address);
            }
        }
    }

public:
    std::string plc_name_;
    bool verbose_{false};

private:
    asio::steady_timer timer_;
    asio::thread_pool& pool_;
    std::shared_ptr<Client> src_;
    std::shared_ptr<Client> hub_;
    MbMapping map_;

    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> last_state_;
    bool first_run_{true};
    uint64_t suppressed_count_{0};
};

static std::shared_ptr<Client> makeClient(std::string t_host, uint16_t t_port) {
    ClientConfig cfg;
    cfg.host = std::move(t_host);
    cfg.port = t_port;
    cfg.response_timeout_ms = 2000;

    auto res = Client::createShared(std::move(cfg));
    if (res.hasError()) {
        fmt::print(stderr, fg(fmt::color::yellow), "Warning: Modbus client setup failed ({}:{}): {}\n", t_host, t_port, res.error());
        return {};
    }
    return res.value();
}

// =============================================================================
// main callback
// =============================================================================
int main_cb(int t_argc, char** tp_argv) {
    if (t_argc < 2) {
        fmt::print(stderr, "Usage: mbproxy <config.json>\n");
        return EXIT_FAILURE;
    }

    const std::string config_path = sgrn::utils::filesystem::expandUserPath(tp_argv[1]);
    auto config_res = parseMbProxyConfig(config_path);
    if (config_res.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "Config Error: {}\n", config_res.error());
        return EXIT_FAILURE;
    }
    auto config = std::move(config_res.value());

    asio::io_context io_ctx;
    asio::thread_pool worker_pool(4);

    auto hub_ = makeClient(config.hub_ip, config.hub_port);
    if (!hub_) {
        fmt::print(stderr, fg(fmt::color::red), "Failed to create Modbus hub client.\n");
        return EXIT_FAILURE;
    }
    fmt::print("Connecting to Modbus Hub at {}:{}...\n", config.hub_ip, config.hub_port);
    if (hub_->ensureConnected().hasError()) {
        fmt::print(stderr, fg(fmt::color::yellow), "Warning: Could not connect to Hub (will retry asynchronously).\n");
    }

    std::vector<std::shared_ptr<MbProxySession>> sessions;
    for (const auto& dev_cfg : config.devices) {
        fmt::print("Connecting to Source Modbus Device [{}] at {}:{}...\n", dev_cfg.name, dev_cfg.ip, dev_cfg.port);
        auto src_ = makeClient(dev_cfg.ip, dev_cfg.port);
        if (!src_) {
            fmt::print(stderr, fg(fmt::color::yellow), "Warning: Could not create client for {} (skipping).\n", dev_cfg.name);
            continue;
        }
        if (src_->ensureConnected().hasError()) {
            fmt::print(stderr, fg(fmt::color::yellow), "Warning: Could not connect to {} (will retry).\n", dev_cfg.name);
        }

        for (const auto& t_map : dev_cfg.mappings) {
            auto session = std::make_shared<MbProxySession>(io_ctx, worker_pool, src_, hub_, t_map);
            session->plc_name_ = dev_cfg.name;
            session->verbose_ = config.verbose;
            session->start();
            sessions.push_back(session);
        }
    }

    if (sessions.empty()) {
        fmt::print(stderr, fg(fmt::color::red), "No valid sessions configured. Exiting.\n");
        return EXIT_FAILURE;
    }

    fmt::print(fg(fmt::color::green), "MbProxy active. Multiplexing {} sessions across {} Modbus devices.\n", sessions.size(),
        config.devices.size());

    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](auto, int) {
        fmt::print("\nShutdown signal received. Cleaning up...\n");
        io_ctx.stop();
    });

    io_ctx.run();
    worker_pool.join();

    return EXIT_SUCCESS;
}

int main(int t_argc, char** tp_argv) {
    return sgrn::utils::app::runMain(t_argc, tp_argv, main_cb, "MbProxy");
}
