#pragma once

#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::gateway::config
{

/**
 * @brief Mapping from a Source Modbus address block to a Hub Modbus address block.
 */
struct MbMapping {
    std::string type{"holding"}; // "holding" or "coil"
    uint16_t src_address{0};
    uint16_t dst_address{0};
    uint16_t count{1};
    int interval_ms{100};
};

/**
 * @brief Configuration for a single Source Modbus PLC.
 */
struct MbDevice {
    std::string name;
    std::string ip;
    uint16_t port{502};
    std::vector<MbMapping> mappings;
};

/**
 * @brief Global configuration for the MbProxy.
 */
struct MbProxyConfig {
    std::string hub_ip{"127.0.0.1"};
    uint16_t hub_port{502};
    std::vector<MbDevice> devices;
    bool verbose{false};
};

/**
 * @brief Parse the MbProxy configuration from a JSON file using RapidJSON.
 */
sgrn::Result<MbProxyConfig, std::string> parseMbProxyConfig(const std::string& t_path);

} // namespace sgrn::gateway::config
