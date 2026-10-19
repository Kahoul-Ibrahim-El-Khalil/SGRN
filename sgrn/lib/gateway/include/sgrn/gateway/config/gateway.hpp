#pragma once

#include <sgrn/gateway/config/datastore.hpp>
#include <sgrn/gateway/config/http.hpp>
#include <sgrn/gateway/config/opcua.hpp>
#include <sgrn/gateway/config/persistence.hpp>
#include <sgrn/gateway/config/s7.hpp>
#include <sgrn/gateway/config/websocket.hpp>
#include <sgrn/scl/types.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace sgrn::gateway::config
{

#if defined(_WIN32)
#include <cstdlib>
#include <filesystem>

inline std::string adaptConfigPath(std::string path) {
    auto get_win_temp = []() -> std::string {
        std::error_code ec;
        auto temp_path = std::filesystem::temp_directory_path(ec);
        if (!ec && !temp_path.empty()) {
            std::string s = temp_path.string();
            while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
                s.pop_back();
            }
            return s;
        }
        const char* env_temp = std::getenv("TEMP");
        if (env_temp && *env_temp != '\0')
            return std::string(env_temp);
        const char* env_tmp = std::getenv("TMP");
        if (env_tmp && *env_tmp != '\0')
            return std::string(env_tmp);
        return "./tmp";
    };

    if (path == "/tmp" || path == "\\tmp") {
        return get_win_temp();
    }
    if (path.rfind("/tmp/", 0) == 0) {
        return get_win_temp() + "/" + path.substr(5);
    }
    if (path.rfind("\\tmp\\", 0) == 0) {
        return get_win_temp() + "\\" + path.substr(5);
    }
    return path;
}
#define SGRN_CONFIG_PATH(p) ::sgrn::gateway::config::adaptConfigPath(p)
#define SGRN_ADAPT_PATH(p) ::sgrn::gateway::config::adaptConfigPath(p)
#else
#define SGRN_CONFIG_PATH(p) (p)
#define SGRN_ADAPT_PATH(p) (p)
#endif

struct ModbusConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{502};
};

struct EthernetIpConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{44818};
};

struct GatewayConfig {
    std::optional<S7Config> s7;
    std::optional<OpcUaConfig> opcua;
    std::optional<HttpConfig> http;
    std::optional<WebSocketConfig> websocket;
    std::optional<DatastoreConnectionConfig> datastore;
    std::optional<ModbusConfig> modbus;
    std::optional<EthernetIpConfig> ethernetip;

    PersistenceConfig persistence;

    ::sgrn::scl::SecurityPolicy security_policy{::sgrn::scl::SecurityPolicy::Relaxed};
    std::string security_script{""};
    std::map<uint16_t, std::string> db_acls;
    std::set<uint16_t> declared_db_numbers;
    std::map<uint16_t, std::string> dirty_trackers;
    std::string state_dir{"./gateway-state"};
    std::string schema_file{""};
    std::string symbols_dir{""};
    int reconnect_ms{5000};
    bool cache_json_north{true};
    /// Telemetry-dispatch threads (GlobalContext io_context: TelemetryBroker
    /// fan-out to WebSocket/OPC-UA/persistence). Default 2.
    int light_threads{2};
    /// Compression/disk-I/O worker pool (PersistenceService, DatastoreBridge).
    /// Default 2.
    int heavy_threads{2};
    bool verbose{false};
    bool debug_incoming{false};
    bool debug_tree{false};
    uint32_t database_rotation_interval_s{86400}; // 24 hours
};

/**
 * @brief Parse a gateway configuration JSON file.
 */
sgrn::Result<GatewayConfig> parseNodeConfig(const std::string& t_path);

constexpr const char example[] = R"({
  "listen": {
    "s7": {
      "ip": "0.0.0.0",
      "port": 102,
      "max_clients": 12,
      "pdu_size": 960,
      "little_endian": true
    },
    "opcua": {
      "ip": "127.0.0.1",
      "port": 4840
    },
    "http": {
      "ip": "127.0.0.1",
      "port": 8000
    },
    "websocket": {
      "ip": "127.0.0.1",
      "port": 8000
    },
    "modbus": {
      "ip": "0.0.0.0",
      "port": 502
    },
    "ethernetip": {
      "ip": "0.0.0.0",
      "port": 44818
    }
  },
  "schema": "./schema.scl",
  "output": {
    "directory": "./gateway-output"
  },
  "cache_json_north": true,
  "reconnect_ms": 5000,
  "threading": {
    "light_threads": 2,
    "heavy_threads": 2
  },
  "persistence": {
    "enabled": false,
    "mode": "changes_with_timestamp",
    "namespaces": [],
    "atomic_window_ms": 10,
    "batch_size": 1000,
    "batch_interval_s": 300,
    "anchor_interval_s": 86400,
    "anchor_change_count": 10000,
    "zstd_level": 5,
    "anchor_zstd_level": 12
  },
  "datastore": {
    "url": "https://localhost:443",
    "public_token": "default-public-token-uuid",
    "private_token": "default-private-token-key-base64",
    "object_name": "default_facility",
    "telemetry_enabled": true,
    "upload_mode": "raw",
    "vfs_remote_dir": "/gateway/snapshots",
    "snapshot_mode": "Anchored",
    "batch_size": 1000,
    "batch_interval_s": 1000,
    "zstd_level": 5,
    "aggressive_zstd_level": 15,
    "enable_aggressive_compression": true
  },
  "security_policy": "strict",
  "security_script": "./configs/security.as"
}
)";

} // namespace sgrn::gateway::config
