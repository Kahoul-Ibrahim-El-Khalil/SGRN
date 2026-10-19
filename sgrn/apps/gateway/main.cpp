#include <sgrn/Result.hpp>

#include <sgrn/gateway/gateway.hpp>
#include <atomic>
#include <chrono>
#include <cxxopts.hpp>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using sgrn::Result;
using sgrn::gateway::GatewayApplication;

namespace
{
constexpr std::string_view MAN_MESSAGE =
    "SGRN Gateway — Multi-Protocol Industrial Gateway\n\n"
    "USAGE:\n"
    "    gateway -c gateway.json [-s schema.scl] [-p policy.as] [--gui] [--headless]\n\n"
    "CONFIG FILE:\n"
    "    gateway.json contains all protocol adapters, security, persistence, and DB mappings.\n"
    "    See sgrn/gateway/config/example.json for full reference.\n"
    "    Note: On Windows, paths starting with /tmp/ (such as \"state_dir\": \"/tmp/sgrn-gateway-state\")\n"
    "    are automatically substituted with the Windows user temporary directory (%TEMP%, e.g.\n"
    "    C:\\Users\\<user>\\AppData\\Local\\Temp) for seamless cross-platform execution.\n\n"
    "SCHEMA:\n"
    "    -s/--schema accepts a .scl file or directory of .scl/.udt files.\n"
    "    Overrides \"schema\" or \"symbols_dir\" in gateway.json.\n\n"
    "SECURITY POLICY:\n"
    "    -p/--policy loads an AngelScript policy script for granular ACLs.\n"
    "    Overrides \"security_script\" in gateway.json.\n\n"
    "HEADLESS MODE:\n"
    "    --headless starts only HTTP+WebSocket adapters (no S7/OPC-UA/Modbus/EIP).\n"
    "    Useful for replay and simulation. HTTP on --http-port, WebSocket at /ws.\n\n"
    "EXAMPLES:\n"
    "    gateway -c gateway.json\n"
    "    gateway -c gateway.json -s schema.scl --gui\n"
    "    gateway -c gateway.json --headless --http-port 8080\n"
    "    gateway --headless -s schema.scl --http-port 8080\n\n"
    "For full manual: gateway --man\n";
}

Result<void, std::string> start(int argc, char** argv) {
    cxxopts::Options options("gateway", "SGRN Multi-Protocol Industrial Gateway");
    options.add_options()("c,config", "Path to gateway.json config file", cxxopts::value<std::string>())("s,schema",
        "SCL schema file or directory",
        cxxopts::value<std::string>())("p,policy", "Security policy script (.as)", cxxopts::value<std::string>())(
        "g,gui", "Open embedded dashboard in browser", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help")("man",
        "Print manual page")("headless", "Run in headless replay mode (no S7/OPC-UA)", cxxopts::value<bool>()->default_value("false"))(
        "http-port", "HTTP port for headless mode", cxxopts::value<uint16_t>()->default_value("8080"))(
        "ws-port", "WebSocket port for headless mode", cxxopts::value<uint16_t>()->default_value("8081"));

    options.parse_positional({"config"});

    try {
        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            fmt::print("{}\n", options.help());
            return {};
        }

        if (result.count("man")) {
            fmt::print("{}\n", MAN_MESSAGE);
            return {};
        }

        GatewayApplication app;

        // Build config from flags
        std::string config_path = result.count("config") ? SGRN_CONFIG_PATH(result["config"].as<std::string>()) : "";
        std::string schema_override = result.count("schema") ? SGRN_CONFIG_PATH(result["schema"].as<std::string>()) : "";
        std::string policy_override = result.count("policy") ? SGRN_CONFIG_PATH(result["policy"].as<std::string>()) : "";
        bool gui_mode = result["gui"].as<bool>();
        bool headless = result["headless"].as<bool>();
        uint16_t http_port = result["http-port"].as<uint16_t>();
        uint16_t ws_port = result["ws-port"].as<uint16_t>();

        // Build minimal config for headless mode
        if (headless) {
            if (schema_override.empty()) {
                fmt::print(stderr, "Error: --schema required in headless mode\n");
                return "schema required";
            }
            app.enableHeadlessMode(http_port, ws_port);
            app.setSchemaOverride(schema_override);
        } else if (config_path.empty()) {
            fmt::print(stderr, "Error: -c/--config <gateway.json> required (or use --headless with --schema)\n");
            return "config required";
        }

        // Load config from file (or minimal headless config)
        if (!headless) {
            SGRN_IF_ERROR_PROPAGATE(app.loadConfig(argc, argv));

            if (!schema_override.empty()) {
                app.setSchemaOverride(schema_override);
            }

            if (!policy_override.empty()) {
                app.setPolicyOverride(policy_override);
            }
        }

        if (gui_mode) {
            app.enableGuiMode();
        }

        SGRN_IF_ERROR_PROPAGATE(app.loadSchema());
        SGRN_IF_ERROR_PROPAGATE(app.initSecurity());
        SGRN_IF_ERROR_PROPAGATE(app.initTwin());
        SGRN_IF_ERROR_PROPAGATE(app.initThreading());
        SGRN_IF_ERROR_PROPAGATE(app.wireTelemetry());
        SGRN_IF_ERROR_PROPAGATE(app.initInfrastructure());
        SGRN_IF_ERROR_PROPAGATE(app.startAdapters());

        app.feedInitialAnchor();
        app.run();
        app.shutdown();
        return {};
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return e.what();
    }
}

int main_cb(int argc, char** argv) {
    if (const Result<void, std::string> r = start(argc, argv); r.hasError()) {
        fmt::print(stderr, "{}\n", r.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int t_argc, char** t_argv) {
    return sgrn::utils::app::runMain(t_argc, t_argv, main_cb, "Gateway");
}
