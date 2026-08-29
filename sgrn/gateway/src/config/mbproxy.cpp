#include <sgrn/gateway/config/mbproxy.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <sstream>

namespace sgrn::gateway::config
{
using sgrn::Result;

Result<MbProxyConfig, std::string> parseMbProxyConfig(const std::string& t_path) {
    std::ifstream ifs(t_path);
    if (!ifs.is_open())
        return Error(fmt::format("Cannot open config file: {}", t_path));

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    std::string content = buffer.str();

    rapidjson::Document doc;
    if (doc.Parse(content.c_str()).HasParseError()) {
        return Error(
            fmt::format("JSON Parse SchemaError: {} at offset {}", rapidjson::GetParseError_En(doc.GetParseError()), doc.GetErrorOffset()));
    }

    if (!doc.IsObject()) {
        return Error(std::string("Config root must be an object"));
    }

    MbProxyConfig cfg;

    if (doc.HasMember("hub") && doc["hub"].IsObject()) {
        const auto& hub = doc["hub"];
        if (hub.HasMember("ip") && hub["ip"].IsString())
            cfg.hub_ip = hub["ip"].GetString();
        if (hub.HasMember("port") && hub["port"].IsUint())
            cfg.hub_port = static_cast<uint16_t>(hub["port"].GetUint());
    }

    if (doc.HasMember("verbose") && doc["verbose"].IsBool()) {
        cfg.verbose = doc["verbose"].GetBool();
    }

    if (doc.HasMember("devices") && doc["devices"].IsArray()) {
        const auto& devices = doc["devices"];
        for (rapidjson::SizeType i = 0; i < devices.Size(); ++i) {
            if (!devices[i].IsObject())
                continue;
            const auto& d = devices[i];

            MbDevice device;
            if (d.HasMember("name") && d["name"].IsString())
                device.name = d["name"].GetString();
            if (d.HasMember("ip") && d["ip"].IsString())
                device.ip = d["ip"].GetString();
            if (d.HasMember("port") && d["port"].IsUint())
                device.port = static_cast<uint16_t>(d["port"].GetUint());

            if (d.HasMember("mappings") && d["mappings"].IsArray()) {
                const auto& maps = d["mappings"];
                for (rapidjson::SizeType j = 0; j < maps.Size(); ++j) {
                    if (!maps[j].IsObject())
                        continue;
                    const auto& m = maps[j];

                    MbMapping mapping;
                    if (m.HasMember("type") && m["type"].IsString())
                        mapping.type = m["type"].GetString();
                    if (m.HasMember("src_address") && m["src_address"].IsUint())
                        mapping.src_address = static_cast<uint16_t>(m["src_address"].GetUint());
                    if (m.HasMember("dst_address") && m["dst_address"].IsUint())
                        mapping.dst_address = static_cast<uint16_t>(m["dst_address"].GetUint());
                    if (m.HasMember("count") && m["count"].IsUint())
                        mapping.count = static_cast<uint16_t>(m["count"].GetUint());
                    if (m.HasMember("interval_ms") && m["interval_ms"].IsInt())
                        mapping.interval_ms = m["interval_ms"].GetInt();

                    device.mappings.push_back(mapping);
                }
            }
            cfg.devices.push_back(device);
        }
    }

    return cfg;
}

} // namespace sgrn::gateway::config
