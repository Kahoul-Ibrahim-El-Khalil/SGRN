#include <sgrn/gateway/config/proxy.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <fstream>
#include <iterator>

namespace sgrn::gateway::config
{
using sgrn::Result;

namespace detail
{

struct DbMappingJson {
    uint16_t src_db{0};
    uint16_t dst_db{0};
    int interval_ms{100};
};

struct PlcSourceJson {
    std::string name;
    std::string ip;
    int rack{0};
    int slot{1};
    std::vector<DbMappingJson> mappings;
};

struct S7ProxyConfigJson {
    struct Hub {
        std::string ip{"127.0.0.1"};
        uint16_t port{102};
    } hub;
    std::string registry;
    std::vector<PlcSourceJson> plcs;
    bool verbose{false};
};

} // namespace detail

} // namespace sgrn::gateway::config

namespace glz
{

template <>
struct meta<sgrn::gateway::config::detail::DbMappingJson> {
    using T = sgrn::gateway::config::detail::DbMappingJson;
    static constexpr auto value = object("src_db", &T::src_db, "dst_db", &T::dst_db, "interval_ms", &T::interval_ms);
};

template <>
struct meta<sgrn::gateway::config::detail::PlcSourceJson> {
    using T = sgrn::gateway::config::detail::PlcSourceJson;
    static constexpr auto value = object("name", &T::name, "ip", &T::ip, "rack", &T::rack, "slot", &T::slot, "mappings", &T::mappings);
};

template <>
struct meta<sgrn::gateway::config::detail::S7ProxyConfigJson::Hub> {
    using T = sgrn::gateway::config::detail::S7ProxyConfigJson::Hub;
    static constexpr auto value = object("ip", &T::ip, "port", &T::port);
};

template <>
struct meta<sgrn::gateway::config::detail::S7ProxyConfigJson> {
    using T = sgrn::gateway::config::detail::S7ProxyConfigJson;
    static constexpr auto value = object("hub", &T::hub, "registry", &T::registry, "plcs", &T::plcs, "verbose", &T::verbose);
};

} // namespace glz

namespace sgrn::gateway::config
{

Result<S7ProxyConfig, std::string> parseProxyConfig(const std::string& t_path) {
    std::ifstream ifs(t_path);
    if (!ifs.is_open()) {
        return Error(fmt::format("Cannot open config file: {}", t_path));
    }

    const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto wire_res = sgrn::json::read<detail::S7ProxyConfigJson>(content);
    if (wire_res.hasError())
        return Error(fmt::format("Failed to deserialize S7 proxy config: {}", wire_res.error().what()));

    const auto& wire = wire_res.value();
    S7ProxyConfig cfg;
    cfg.hub_ip = wire.hub.ip;
    cfg.hub_port = wire.hub.port;
    cfg.registry_path = wire.registry;
    cfg.verbose = wire.verbose;
    cfg.plcs.reserve(wire.plcs.size());
    for (const auto& source_wire : wire.plcs) {
        PlcSource source;
        source.name = source_wire.name;
        source.ip = source_wire.ip;
        source.rack = source_wire.rack;
        source.slot = source_wire.slot;
        source.mappings.reserve(source_wire.mappings.size());
        for (const auto& mapping_wire : source_wire.mappings)
            source.mappings.push_back({mapping_wire.src_db, mapping_wire.dst_db, mapping_wire.interval_ms});
        cfg.plcs.push_back(std::move(source));
    }

    return cfg;
}

} // namespace sgrn::gateway::config
