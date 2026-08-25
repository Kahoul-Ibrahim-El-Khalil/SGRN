#include <fmt/core.h>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/http/path.hpp>
#include <sgrn/gateway/common/json_helper.hpp>
#include <sgrn/gateway/core/TreeCacheEngine.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/TypeDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/rapidjson.hpp>
#include <sgrn/utils/strings.hpp>

#include <sgrn/scl/functions/modbus.hpp>
using sgrn::gateway::common::json_helper::buildArrayResponse;
namespace sgrn::gateway::adapters
{

void HttpAdapter::handleGetModbusRegistry(const httplib::Request&, httplib::Response& t_res) {
    if (!modbus_map_) {
        t_res.status = 404;
        t_res.set_content(R"({"error":"Modbus support not initialized or no virtual register map built"})", "application/json");
        return;
    }
    t_res.set_content(::sgrn::scl::serializeModbusMapToJson(*modbus_map_), "application/json");
}

void HttpAdapter::handleGetRegistryTypes(const httplib::Request&, httplib::Response& t_res) {
    t_res.set_content(std::string(::sgrn::gateway::twin::kS7TypeDictionaryJson), "application/json");
}

void HttpAdapter::handleGetRegistry(const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry) {
    std::optional<uint16_t> db_num;
    bool headers_only = false;
    if (t_req.has_param("db")) {
        try {
            db_num = static_cast<uint16_t>(std::stoi(t_req.get_param_value("db")));
        } catch (const std::invalid_argument& e) {
            t_res.status = 400;
            t_res.set_content(fmt::format(R"({{"error":"Invalid 'db' parameter: {}"}})", e.what()), "application/json");
            return;
        } catch (const std::out_of_range& e) {
            t_res.status = 400;
            t_res.set_content(fmt::format(R"({{"error":"'db' parameter out of range: {}"}})", e.what()), "application/json");
            return;
        }
    }
    if (t_req.has_param("headers"))
        headers_only = (t_req.get_param_value("headers") == "true");
    t_res.set_content(t_registry.toJson(db_num, headers_only), "application/json");
}

void HttpAdapter::handleGetData(
    const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry, twin::PlcMemory& t_memory) {
    std::string path = t_req.matches[1];
    if (!path.empty() && path.back() == '/')
        path.pop_back();

    if (path.empty()) {
        if (!isAuthorizedField(t_req, std::nullopt, "", false)) {
            t_res.status = 403;
            t_res.set_content(R"({"error":"Forbidden: Not authorised to read full twin"})", "application/json");
            return;
        }
        t_res.set_content(t_memory.getDigitalTwinJsonString(), "application/json");
        return;
    }

    auto [schema, field_path, array_index] = resolveSemanticPath(sgrn::utils::strings::tokenize(path, '/'), t_registry);

    // ACL Check
    std::optional<uint16_t> db_num = schema ? std::optional<uint16_t>(schema->db_number) : std::nullopt;
    if (!isAuthorizedField(t_req, db_num, field_path, false)) {
        t_res.status = 403;
        t_res.set_content(fmt::format(R"({{"error":"Forbidden: IP {} is not authorised to read path '{}'"}})", t_req.remote_addr, path),
            "application/json");
        return;
    }

    // Tier 4: Unify REST via TreeCacheEngine
    auto tree_path = sgrn::gateway::twin::TreePath::fromSlashed(path);
    auto cached = sgrn::gateway::core::TreeCacheEngine::instance().get(tree_path, *t_memory.state());
    if (cached && *cached != "null" && *cached != "{}") {
        t_res.set_content(*cached, "application/json");
        return;
    }

    if (schema) {
        if (array_index.has_value()) {
            auto arr_res = t_memory.getFieldValue(schema->db_number, field_path);
            if (!arr_res.hasError()) {
                rapidjson::Document arr_doc;
                if (!arr_doc.Parse(arr_res.value().c_str()).HasParseError() && arr_doc.IsArray()) {
                    const size_t idx = *array_index;
                    if (idx < arr_doc.GetArray().Size()) {
                        rapidjson::StringBuffer sb;
                        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
                        arr_doc[static_cast<rapidjson::SizeType>(idx)].Accept(w);
                        t_res.set_content(sb.GetString(), "application/json");
                        return;
                    }
                    t_res.status = 416;
                    t_res.set_content(
                        fmt::format(R"X({{"error":"Array index {} out of range (size={})"}})X", idx, arr_doc.GetArray().Size()),
                        "application/json");
                    return;
                }
            }
            t_res.status = 404;
            t_res.set_content(fmt::format(R"({{"error":"Array field '{}' not found or not an array"}})", field_path), "application/json");
            return;
        }

        std::string db_json = t_memory.getDbJsonString(schema->db_number);
        if (!db_json.empty()) {
            if (field_path.empty()) {
                t_res.set_content(db_json, "application/json");
                return;
            }
            if (auto sub = sgrn::utils::rapidjson::extractSubtree(db_json, field_path)) {
                t_res.set_content(*sub, "application/json");
                return;
            }
        }
    }

    t_res.status = 404;
    t_res.set_content("Data not found: " + path, "text/plain");
}

void HttpAdapter::handleGetConnections(const httplib::Request&, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db) {
    auto conns_res = t_db.getConnections();
    if (conns_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", conns_res.error()), "application/json");
        return;
    }
    auto conns = std::move(conns_res.value());
    t_res.set_content(buildArrayResponse(conns.size(),
                          [&](rapidjson::Value& item, auto& alloc, size_t i) {
                              const auto& c = conns[i];
                              item.AddMember("type", rapidjson::Value(c.type.c_str(), alloc), alloc);
                              item.AddMember("ip", rapidjson::Value(c.remote_ip.c_str(), alloc), alloc);
                              item.AddMember("endpoint", rapidjson::Value(c.endpoint.c_str(), alloc), alloc);
                              item.AddMember("first_seen", c.first_seen, alloc);
                              item.AddMember("last_seen", c.last_seen, alloc);
                              item.AddMember("event_count", c.event_count, alloc);
                          }),
        "application/json");
}

void HttpAdapter::handleGetDbHistory(const httplib::Request&, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db) {
    auto hist_res = t_db.exportHistoricalDataToJson();
    if (hist_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", hist_res.error()), "application/json");
        return;
    }
    t_res.set_content(std::move(hist_res.value()), "application/json");
}

void HttpAdapter::handleGetDbSessions(const httplib::Request&, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db) {
    auto sessions_res = t_db.getActiveSessions();
    if (sessions_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", sessions_res.error()), "application/json");
        return;
    }
    auto sessions = std::move(sessions_res.value());
    t_res.set_content(buildArrayResponse(sessions.size(),
                          [&](rapidjson::Value& item, auto& alloc, size_t i) {
                              const auto& s = sessions[i];
                              item.AddMember("id", s.id, alloc);
                              item.AddMember("ip", rapidjson::Value(s.ip.c_str(), alloc), alloc);
                              item.AddMember("connect_time", s.connect_time, alloc);
                              item.AddMember("bytes_sent", s.bytes_sent, alloc);
                              item.AddMember("bytes_received", s.bytes_received, alloc);
                          }),
        "application/json");
}

void HttpAdapter::handleGetDbLogs(const httplib::Request& t_req, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db) {
    int limit = 100;
    if (t_req.has_param("limit")) {
        try {
            limit = std::stoi(t_req.get_param_value("limit"));
        } catch (const std::invalid_argument& e) {
            t_res.status = 400;
            t_res.set_content(fmt::format(R"({{"error":"Invalid 'limit' parameter: {}"}})", e.what()), "application/json");
            return;
        } catch (const std::out_of_range& e) {
            t_res.status = 400;
            t_res.set_content(fmt::format(R"({{"error":"'limit' parameter out of range: {}"}})", e.what()), "application/json");
            return;
        }
    }
    auto logs_res = t_db.getLogs(limit);
    if (logs_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", logs_res.error()), "application/json");
        return;
    }
    auto logs = std::move(logs_res.value());
    t_res.set_content(buildArrayResponse(logs.size(),
                          [&](rapidjson::Value& item, auto& alloc, size_t i) {
                              const auto& l = logs[i];
                              item.AddMember("ts", l.timestamp, alloc);
                              item.AddMember("level", rapidjson::Value(l.level.c_str(), alloc), alloc);
                              item.AddMember("msg", rapidjson::Value(l.message.c_str(), alloc), alloc);
                          }),
        "application/json");
}

} // namespace sgrn::gateway::adapters
