#include <sgrn/gateway/config/mbproxy.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <fstream>
#include <iterator>

namespace sgrn::gateway::config
{
using sgrn::Result;

namespace detail
{

struct MbMappingJson {
    std::string type{"holding"};
    uint16_t src_address{0};
    uint16_t dst_address{0};
    uint16_t count{1};
    int interval_ms{100};
};

struct MbDeviceJson {
    std::string name;
    std::string ip;
    uint16_t port{502};
    std::vector<MbMappingJson> mappings;
};

struct MbProxyConfigJson {
    struct Hub {
        std::string ip{"127.0.0.1"};
        uint16_t port{502};
    } hub;
    std::vector<MbDeviceJson> devices;
    bool verbose{false};
};

} // namespace detail

} // namespace sgrn::gateway::config

namespace sgrn::gateway::config
{

Result<MbProxyConfig, std::string> parseMbProxyConfig(const std::string& t_path) {
    std::ifstream ifs(t_path);
    RETURN_ERROR_IF(!ifs.is_open(), fmt::format("Cannot open config file: {}", t_path));

    const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto wire_res = sgrn::json::read<detail::MbProxyConfigJson>(content);
    RETURN_ERROR_IF(wire_res.hasError(), fmt::format("Failed to deserialize Modbus proxy config: {}", wire_res.error().what()));

    const auto& wire = wire_res.value();
    MbProxyConfig cfg;
    cfg.hub_ip = wire.hub.ip;
    cfg.hub_port = wire.hub.port;
    cfg.verbose = wire.verbose;
    cfg.devices.reserve(wire.devices.size());
    for (const auto& device_wire : wire.devices) {
        MbDevice device;
        device.name = device_wire.name;
        device.ip = device_wire.ip;
        device.port = device_wire.port;
        device.mappings.reserve(device_wire.mappings.size());
        for (const auto& mapping_wire : device_wire.mappings)
            device.mappings.push_back(
                {mapping_wire.type, mapping_wire.src_address, mapping_wire.dst_address, mapping_wire.count, mapping_wire.interval_ms});
        cfg.devices.push_back(std::move(device));
    }

    return cfg;
}

} // namespace sgrn::gateway::config
