#include <sgrn/gateway/config/proxy.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <sstream>

namespace sgrn::gateway::config
{
using sgrn::Result;

Result<S7ProxyConfig, std::string> parseProxyConfig(const std::string& t_path) {
    std::ifstream ifs(t_path);
    if (!ifs.is_open()) {
        return Error(fmt::format("Cannot open config file: {}", t_path));
    }
    std::stringstream buffer;
    buffer << ifs.rdbuf();
    std::string content = buffer.str();

    rapidjson::Document doc;
    if (doc.Parse(content.c_str()).HasParseError()) {
        return Error(
            fmt::format("JSON Parse Error: {} at offset {}", rapidjson::GetParseError_En(doc.GetParseError()), doc.GetErrorOffset()));
    }

    if (!doc.IsObject()) {
        return Error("Config root must be an object");
    }

    S7ProxyConfig cfg;

    if (doc.HasMember("hub") && doc["hub"].IsObject()) {
        const auto& hub = doc["hub"];
        if (hub.HasMember("ip") && hub["ip"].IsString())
            cfg.hub_ip = hub["ip"].GetString();
        if (hub.HasMember("port") && hub["port"].IsUint())
            cfg.hub_port = static_cast<uint16_t>(hub["port"].GetUint());
    }

    if (doc.HasMember("registry") && doc["registry"].IsString()) {
        cfg.registry_path = doc["registry"].GetString();
    }

    if (doc.HasMember("verbose") && doc["verbose"].IsBool()) {
        cfg.verbose = doc["verbose"].GetBool();
    }

    if (doc.HasMember("plcs") && doc["plcs"].IsArray()) {
        const auto& plcs = doc["plcs"];
        for (rapidjson::SizeType i = 0; i < plcs.Size(); ++i) {
            if (!plcs[i].IsObject())
                continue;
            const auto& p = plcs[i];

            PlcSource source;
            if (p.HasMember("name") && p["name"].IsString())
                source.name = p["name"].GetString();
            if (p.HasMember("ip") && p["ip"].IsString())
                source.ip = p["ip"].GetString();
            if (p.HasMember("rack") && p["rack"].IsInt())
                source.rack = p["rack"].GetInt();
            if (p.HasMember("slot") && p["slot"].IsInt())
                source.slot = p["slot"].GetInt();

            if (p.HasMember("mappings") && p["mappings"].IsArray()) {
                const auto& maps = p["mappings"];
                for (rapidjson::SizeType j = 0; j < maps.Size(); ++j) {
                    if (!maps[j].IsObject())
                        continue;
                    const auto& m = maps[j];

                    DbMapping mapping;
                    if (m.HasMember("src_db") && m["src_db"].IsUint())
                        mapping.src_db = static_cast<uint16_t>(m["src_db"].GetUint());
                    if (m.HasMember("dst_db") && m["dst_db"].IsUint())
                        mapping.dst_db = static_cast<uint16_t>(m["dst_db"].GetUint());
                    if (m.HasMember("interval_ms") && m["interval_ms"].IsInt())
                        mapping.interval_ms = m["interval_ms"].GetInt();

                    source.mappings.push_back(mapping);
                }
            }
            cfg.plcs.push_back(source);
        }
    }

    return cfg;
}

} // namespace sgrn::gateway::config
