#include <fmt/core.h>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/http/errors.hpp>
#include <sgrn/gateway/adapters/http/path.hpp>
#include <sgrn/gateway/core/TreeCacheEngine.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/TypeDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <sgrn/utils/json-ops.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/strings.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sgrn/scl/functions/modbus.hpp>
namespace sgrn::gateway::adapters
{

namespace
{

struct ConnectionJson {
    std::string type;
    std::string ip;
    std::string endpoint;
    int64_t first_seen;
    int64_t last_seen;
    int event_count;
};

struct SessionJson {
    int id;
    std::string ip;
    int64_t connect_time;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

struct LogJson {
    int64_t ts;
    std::string level;
    std::string msg;
};

template <class Response, class Rows>
void setJsonArrayResponse(Response& t_res, const Rows& t_rows) {
    auto serialized = sgrn::json::write(t_rows);
    if (serialized.hasError()) {
        t_res.status = 500;
        t_res.set_content(R"JSON({"error":"failed to serialize JSON response"})JSON", "application/json");
        return;
    }
    t_res.set_content(std::move(serialized.value()), "application/json");
}

} // namespace

void HttpAdapter::handleGetModbusRegistry(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (!modbus_map_) {
        t_res.status = 404;
        t_res.set_content(R"({"error":"Modbus support not initialized or no virtual register map built"})", "application/json");
        return;
    }
    t_res.set_content(::sgrn::scl::serializeModbusMapToJson(*modbus_map_), "application/json");
}

void HttpAdapter::handleGetRegistryTypes(const http::HttpRequest&, http::HttpResponse& t_res) {
    t_res.set_content(std::string(::sgrn::gateway::twin::kS7TypeDictionaryJson), "application/json");
}

void HttpAdapter::handleGetRegistry(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    const PlcSchemaStore& t_registry = *refs_.registry;
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

void HttpAdapter::handleGetData(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    const PlcSchemaStore& t_registry = *refs_.registry;
    twin::PlcMemory& t_memory = *refs_.memory;
    std::string path = t_req.path;
    if (!path.empty() && path.back() == '/')
        path.pop_back();

    if (path.empty()) {
        if (!isAuthorizedField(t_req, std::nullopt, "", false)) {
            t_res.status = 403;
            t_res.set_content(R"({"error":"Forbidden: Not authorised to read full twin"})", "application/json");
            return;
        }
        // Tier 4 unified: full-twin JSON comes from the TreeCache root entry
        // (per-DB version key) instead of re-serializing every request.
        auto cached = sgrn::gateway::core::TreeCacheEngine::instance().getRoot(*t_memory.state());
        if (cached && *cached != "null" && *cached != "{}") {
            t_res.set_content(*cached, "application/json");
            return;
        }
        t_res.set_content(t_memory.getDigitalTwinJsonString(), "application/json");
        return;
    }

    auto [schema, field_path, array_rest_path, array_index] = resolveSemanticPath(sgrn::utils::strings::tokenize(path, '/'), t_registry);

    // ACL Check
    std::optional<uint16_t> db_num = schema ? std::optional<uint16_t>(schema->db_number) : std::nullopt;
    if (!isAuthorizedField(t_req, db_num, field_path, false)) {
        t_res.status = 403;
        t_res.set_content(fmt::format(R"({{"error":"Forbidden: IP {} is not authorised to read path '{}'"}})", t_req.remote_ip, path),
            "application/json");
        return;
    }

    // Tier 4: Unify REST via TreeCacheEngine
    auto tree_path = ::sgrn::common::TreePath::fromSlashed(path);
    auto cached = sgrn::gateway::core::TreeCacheEngine::instance().get(tree_path, *t_memory.state());
    if (cached && *cached != "null" && *cached != "{}") {
        t_res.set_content(*cached, "application/json");
        return;
    }

    if (schema) {
        if (array_index.has_value()) {
            auto arr_res = t_memory.getFieldValue(schema->db_number, field_path);
            if (arr_res.hasError()) {
                t_res.status = http::toHttpStatus(arr_res.error());
                t_res.set_content(fmt::format(R"({{"error":"Field '{}' not found"}})", field_path), "application/json");
                return;
            }
            if (!arr_res.hasError()) {
                sgrn::json::dom::Document arr_doc;
                if (!arr_doc.parse(arr_res.value().c_str()).hasParseError() && arr_doc.is_array()) {
                    const size_t idx = *array_index;
                    if (idx < arr_doc.elements().size()) {
                        sgrn::json::dom::Value element = sgrn::json::dom::Value::view(arr_doc.rawValue().get_array().at(idx));

                        // If there's a rest path, extract the nested field
                        if (!array_rest_path.empty()) {
                            const auto segments = sgrn::gateway::adapters::splitSlashes(array_rest_path);
                            sgrn::json::dom::Value* current = &element;
                            sgrn::json::dom::Value found_value;
                            for (size_t i = 0; i < segments.size(); ++i) {
                                if (!current->is_object()) {
                                    t_res.status = 422;
                                    t_res.set_content(
                                        fmt::format(
                                            R"X({{"error":"Field '{}' not found in array element (not an object)"}})X", array_rest_path),
                                        "application/json");
                                    return;
                                }
                                // Case-insensitive field lookup
                                bool found = false;
                                for (auto it = current->memberBegin(); it != current->memberEnd(); ++it) {
                                    if (strcasecmp(it->name.getString(), segments[i].data()) == 0) {
                                        found_value.rebindTo(it->value);
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    t_res.status = 422;
                                    t_res.set_content(
                                        fmt::format(R"X({{"error":"Field '{}' not found in array element"}})X", segments[i].data()),
                                        "application/json");
                                    return;
                                }
                                if (i == segments.size() - 1) {
                                    sgrn::json::dom::StringBuffer sb;
                                    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
                                    found_value.accept(w);
                                    t_res.set_content(sb.getString(), "application/json");
                                } else {
                                    current = &found_value;
                                }
                            }
                        } else {
                            sgrn::json::dom::StringBuffer sb;
                            sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
                            element.accept(w);
                            t_res.set_content(sb.getString(), "application/json");
                        }
                        return;
                    }
                    t_res.status = 416;
                    t_res.set_content(
                        fmt::format(R"X({{"error":"Array index {} out of range (size={})"}})X", idx, arr_doc.elements().size()),
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
            if (auto sub = sgrn::utils::json::extractSubtree(db_json, field_path)) {
                t_res.set_content(*sub, "application/json");
                return;
            }
        }
    }

    t_res.status = 404;
    t_res.set_content("Data not found: " + path, "text/plain");
}

void HttpAdapter::handleGetConnections(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (!refs_.db) {
        t_res.status = 500;
        t_res.set_content(R"({"error":"Database not initialized"})", "application/json");
        return;
    }
    auto conns_res = refs_.db->getConnections();
    if (conns_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", conns_res.error()), "application/json");
        return;
    }
    auto conns = std::move(conns_res.value());
    std::vector<ConnectionJson> rows;
    rows.reserve(conns.size());
    for (auto& c : conns) {
        rows.push_back({std::move(c.type), std::move(c.remote_ip), std::move(c.endpoint), c.first_seen, c.last_seen, c.event_count});
    }
    setJsonArrayResponse(t_res, rows);
}

void HttpAdapter::handleGetDbHistory(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (!refs_.db) {
        t_res.status = 500;
        t_res.set_content(R"({"error":"Database not initialized"})", "application/json");
        return;
    }
    auto hist_res = refs_.db->exportHistoricalDataToJson();
    if (hist_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", hist_res.error()), "application/json");
        return;
    }
    t_res.set_content(std::move(hist_res.value()), "application/json");
}

void HttpAdapter::handleGetDbSessions(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (!refs_.db) {
        t_res.status = 500;
        t_res.set_content(R"({"error":"Database not initialized"})", "application/json");
        return;
    }
    auto sessions_res = refs_.db->getActiveSessions();
    if (sessions_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", sessions_res.error()), "application/json");
        return;
    }
    auto sessions = std::move(sessions_res.value());
    std::vector<SessionJson> rows;
    rows.reserve(sessions.size());
    for (auto& session : sessions) {
        rows.push_back({session.id, std::move(session.ip), session.connect_time, session.bytes_sent, session.bytes_received});
    }
    setJsonArrayResponse(t_res, rows);
}

void HttpAdapter::handleGetDbLogs(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!refs_.db) {
        t_res.status = 500;
        t_res.set_content(R"({"error":"Database not initialized"})", "application/json");
        return;
    }
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
    auto logs_res = refs_.db->getLogs(limit);
    if (logs_res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", logs_res.error()), "application/json");
        return;
    }
    auto logs = std::move(logs_res.value());
    std::vector<LogJson> rows;
    rows.reserve(logs.size());
    for (auto& log : logs) {
        rows.push_back({log.timestamp, std::move(log.level), std::move(log.message)});
    }
    setJsonArrayResponse(t_res, rows);
}

} // namespace sgrn::gateway::adapters
