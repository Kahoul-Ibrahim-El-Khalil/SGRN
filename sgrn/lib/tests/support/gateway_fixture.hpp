#pragma once

// gateway_fixture.hpp — spawn gateway + s7shell for C++ gateway integration
// tests. Port of the GatewayFixture recipe in tests/run_tests.py with one
// deliberate improvement: ports are OS-assigned instead of the fixed
// 8080/4840/8102 (which collide when several builds/tests run at once).
//
// Lifecycle mirrors run_tests.py exactly:
//   - temp state dir (never the source tree: the product default is
//     CWD-relative './gateway-state')
//   - security.as copied from the simulation dir (allow-all fallback)
//   - gateway.json with the `listen.*` shape the config reader accepts
//     (websocket shares the HTTP listener)
//   - gateway on argv[1] = config path; s7shell on the simulation script with
//     cwd = simulation dir (scripts load 'schema.scl' relatively) and
//     SGRN_GATEWAY_WS_URL pointing at our randomized HTTP port
//   - readiness = GET /endpoints → 200 (≤15s)
//
// Binary selection: SGRN_GATEWAY_BIN / SGRN_S7SHELL_BIN, else the release
// preset build, else the static build. Missing binaries → available()==false
// + SKIP semantics.

#include "test_util.hpp"

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sgrn::test
{

/// Per-test knobs beyond the shared defaults. The extra southbound listeners
/// mirror run_tests.py's `listen.*` handling: the gateway only boots a
/// protocol adapter when its `listen.<proto>` block is present in the config.
struct GatewayFixtureOptions {
    bool spawn_simulation = true; ///< run s7shell simulation.as next to the gateway
    bool modbus_listener = false; ///< also write listen.modbus (randomized port)
    bool eip_listener = false;    ///< also write listen.ethernetip (randomized port)
};

class GatewayFixture {
public:
    explicit GatewayFixture(const std::string& t_simulation = "", const GatewayFixtureOptions& t_options = {}) {
        // Per-test name wins (gateway tests opt into `enums` / `tennessee`);
        // env stays as the orchestrator-level override and default fallback.
        const char* env_sim = ::getenv("SGRN_SIMULATION");
        const std::string sim = !t_simulation.empty() ? t_simulation : (env_sim && *env_sim) ? env_sim : "gas_processing";
        m_options = t_options;

        m_repo_sim_dir = repoRoot() + "/sgrn/lib/gateway/simulations/" + sim;
        if (!isDirectory(m_repo_sim_dir) || !pathExists(m_repo_sim_dir + "/schema.scl")) {
            m_skip_reason = "simulation dir or schema.scl missing: " + m_repo_sim_dir;
            return;
        }

        const std::string gateway_bin = ::getenv("SGRN_GATEWAY_BIN") ? ::getenv("SGRN_GATEWAY_BIN") : findApp("gateway");
        const std::string s7shell_bin = ::getenv("SGRN_S7SHELL_BIN") ? ::getenv("SGRN_S7SHELL_BIN") : findApp("s7shell");
        if (gateway_bin.empty()) {
            m_skip_reason = "gateway binary not found (build a preset into .build/<preset> or set SGRN_GATEWAY_BIN)";
            return;
        }
        if (s7shell_bin.empty()) {
            m_skip_reason = "s7shell binary not found (build a preset into .build/<preset> or set SGRN_S7SHELL_BIN)";
            return;
        }

        m_state_dir = makeTempDir("sgrn-gateway");

        // security.as: repo policy wins; else an allow-all script like the
        // Python fixture.
        const std::string policy = m_state_dir + "/security.as";
        if (pathExists(m_repo_sim_dir + "/security.as"))
            writeFile(policy, readFile(m_repo_sim_dir + "/security.as"));
        else
            writeFile(policy, "void setup() { http().allow(); opcua().allow(); websocket().allow(); }\n");

        m_http_port = freePort();
        m_opcua_port = freePort();
        m_s7_port = freePort();
        m_modbus_port = freePort();
        m_eip_port = freePort();
        if (m_http_port <= 0 || m_opcua_port <= 0 || m_s7_port <= 0) {
            m_skip_reason = "no free loopback ports available";
            teardown();
            return;
        }

        // The `listen.*` shape written by tests/run_tests.py (accepted by the
        // config reader; websocket shares the HTTP listener). Adapter-server
        // listeners (`listen.modbus` / future `listen.ethernetip`) only boot
        // when the block is present — tests opt in via the options.
        const std::string config_fmt =
            R"({
  "listen": {
    "s7": {"port": $S7$},
    "opcua": {"port": $OPCUA$},
    "http": {"port": $HTTP$},
    "websocket": {"port": $WS$}$EXTRA_LISTENERS$
  },
  "state_dir": "$STATE$",
  "schema": "$SCHEMA$",
  "security_script": "$POLICY$"
})";

        std::string extra_listeners;
        if (m_options.modbus_listener && m_modbus_port > 0)
            extra_listeners += fmtModbusListener(m_modbus_port);
        if (m_options.eip_listener && m_eip_port > 0)
            extra_listeners += fmtEipListener(m_eip_port);

        std::string cfg = config_fmt;
        for (auto [key, value] : std::vector<std::pair<std::string, std::string>>{
                 {"$S7$", std::to_string(m_s7_port)},
                 {"$OPCUA$", std::to_string(m_opcua_port)},
                 {"$HTTP$", std::to_string(m_http_port)},
                 {"$WS$", std::to_string(m_http_port)}, // websocket shares the HTTP listener
                 {"$EXTRA_LISTENERS$", extra_listeners},
                 {"$STATE$", m_state_dir + "/gateway-state"},
                 {"$SCHEMA$", m_repo_sim_dir + "/schema.scl"},
                 {"$POLICY$", policy},
             }) {
            std::size_t at;
            while ((at = cfg.find(key)) != std::string::npos)
                cfg.replace(at, key.size(), value);
        }
        writeFile(m_state_dir + "/gateway.json", cfg);

        // gateway process
        SpawnResult gw_res = spawnProcess({gateway_bin, m_state_dir + "/gateway.json"}, {}, {}, m_state_dir + "/gateway.log");
        if (gw_res.pid < 0) {
            m_skip_reason = std::string("cannot spawn gateway: ") + gw_res.error;
            teardown();
            return;
        }
        m_gateway = std::make_unique<Process>(gw_res);

        // s7shell soft-PLC / simulation script (optional: southbound tests run
        // the bare gateway so nothing else contends for the DB arena)
        if (m_options.spawn_simulation) {
            SpawnResult sim_res = spawnProcess(
                {s7shell_bin, "simulation.as"}, m_repo_sim_dir, {{"SGRN_GATEWAY_WS_URL", wsUrl()}}, m_state_dir + "/s7shell.log");
            if (sim_res.pid < 0) {
                std::printf("NOTE: s7shell not spawned (%s); registry checks still run off schema.scl\n", sim_res.error.c_str());
            } else {
                m_s7shell = std::make_unique<Process>(sim_res);
            }
        }

        // readiness: GET /endpoints → 200, ≤15s
        httplib::Client http("127.0.0.1", m_http_port);
        http.set_connection_timeout(1);
        for (int i = 0; i < 150; ++i) {
            if (!m_gateway->live()) {
                m_skip_reason = "gateway exited during startup (log: " + m_state_dir + "/gateway.log)";
                teardown();
                return;
            }
            if (auto res = http.Get("/endpoints"); res && res->status == 200) {
                m_ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_ready) {
            m_skip_reason = "gateway failed to become ready on /endpoints within 15s (log: " + m_state_dir + "/gateway.log)";
            teardown();
            return;
        }

        m_available = true;
    }

    ~GatewayFixture() {
        teardown();
    }

    GatewayFixture(const GatewayFixture&) = delete;
    GatewayFixture& operator=(const GatewayFixture&) = delete;

    bool available() const {
        return m_available;
    }
    const std::string& skipReason() const {
        return m_skip_reason;
    }

    int httpPort() const {
        return m_http_port;
    }
    int opcuaPort() const {
        return m_opcua_port;
    }
    int s7Port() const {
        return m_s7_port;
    }
    int modbusPort() const {
        return m_options.modbus_listener ? m_modbus_port : 0;
    }
    int eipPort() const {
        return m_options.eip_listener ? m_eip_port : 0;
    }
    std::string wsUrl() const {
        return "ws://127.0.0.1:" + std::to_string(m_http_port) + "/ws";
    }
    const std::string& stateDir() const {
        return m_state_dir;
    }
    bool simulationLive() const {
        return m_s7shell && m_s7shell->live();
    }

private:
    static std::string fmtModbusListener(int t_port) {
        return std::string(",\n    \"modbus\": {\"ip\": \"127.0.0.1\", \"port\": ") + std::to_string(t_port) + "}";
    }

    static std::string fmtEipListener(int t_port) {
        return std::string(",\n    \"ethernetip\": {\"ip\": \"127.0.0.1\", \"port\": ") + std::to_string(t_port) + "}";
    }

    static std::string findApp(const char* t_app) {
        const std::string rel = std::string("sgrn/apps/") + t_app + "/" + t_app;
        for (const char* preset : {"linux-static-release", "linux-static"})
            if (const std::string p = repoRoot() + "/.build/" + preset + "/" + rel; pathExists(p))
                return p;
        return {};
    }

    void teardown() {
        m_available = false;
        if (m_s7shell)
            m_s7shell->stop();
        if (m_gateway)
            m_gateway->stop();
        if (!m_state_dir.empty()) {
            if (::getenv("SGRN_KEEP_STATE") != nullptr)
                std::printf("NOTE: state dir retained: %s\n", m_state_dir.c_str());
            else {
                std::error_code ec;
                std::filesystem::remove_all(m_state_dir, ec);
                m_state_dir.clear();
            }
        }
    }

    std::string m_repo_sim_dir;
    std::string m_state_dir;
    GatewayFixtureOptions m_options;
    int m_http_port = 0;
    int m_opcua_port = 0;
    int m_s7_port = 0;
    int m_modbus_port = 0;
    int m_eip_port = 0;
    std::unique_ptr<Process> m_gateway;
    std::unique_ptr<Process> m_s7shell;

    bool m_ready = false;
    bool m_available = false;
    std::string m_skip_reason;
};

} // namespace sgrn::test
