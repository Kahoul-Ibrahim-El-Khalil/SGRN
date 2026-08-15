#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <string>
#include <vector>
namespace sgrn::gateway::config
{

/**
 * @brief Mapping from a Source PLC DB to a Hub DB.
 */
struct DbMapping {
    uint16_t src_db{0};
    uint16_t dst_db{0};
    int interval_ms{100};
};

/**
 * @brief Configuration for a single Source PLC.
 */
struct PlcSource {
    std::string name;
    std::string ip;
    int rack{0};
    int slot{1};
    std::vector<DbMapping> mappings;
};

/**
 * @brief Global configuration for the S7Proxy.
 */
struct S7ProxyConfig {
    std::string hub_ip{"127.0.0.1"};
    uint16_t hub_port{102};
    std::string registry_path;
    std::vector<PlcSource> plcs;
    bool verbose{false};
};

/**
 * @brief Parse the S7Proxy configuration from a JSON file using RapidJSON.
 */
sgrn::Result<S7ProxyConfig, std::string> parseProxyConfig(const std::string& t_path);

} // namespace sgrn::gateway::config
