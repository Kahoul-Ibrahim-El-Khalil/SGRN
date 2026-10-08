// tags_handlers.cpp — discrete (TIA-style) tag endpoints for HttpAdapter.
//
//   GET  /tags            — [{"name","table","type","address"}] (empty when unhooked)
//   GET  /tags/<name>     — current value as JSON (404 unknown)
//   POST /tags/<name>     — write JSON value; UDT partials merge (200 {"ok":true})
//   PUT  /tags/<name>     — same write path as POST
//
// Tag memory lives in PlcRuntime (discrete arenas + twin aliases), which the
// twin-backed adapter does not own — owners with a runtime (s7shell Gateway /
// HttpServer bindings) hook it up via setTagAccess(). Without hookup the list
// is empty and reads/writes 404, so full-gateway behavior is unchanged.
#include <fmt/core.h>
#include <sgrn/common/json_helper.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/twin/twin.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace sgrn::gateway::adapters
{

namespace
{
// Minimal %XX decoder for tag names arriving URL-encoded (Crow leaves route
// captures raw, so "Motor Speed" arrives as "Motor%20Speed").
std::string percentDecode(const std::string& t_in) {
    std::string out;
    out.reserve(t_in.size());
    for (size_t i = 0; i < t_in.size(); ++i) {
        if (t_in[i] == '%' && i + 2 < t_in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(t_in[i + 1]);
            const int lo = hex(t_in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(t_in[i] == '+' ? ' ' : t_in[i]);
    }
    return out;
}

std::string jsonQuote(const std::string& t_raw) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.String(t_raw.c_str(), static_cast<rapidjson::SizeType>(t_raw.size()));
    return sb.GetString();
}
} // namespace

void HttpAdapter::handleGetTags(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!isAuthorizedField(t_req, std::nullopt, "", false)) {
        t_res.status = 403;
        t_res.set_content(R"({"error":"Forbidden"})", "application/json");
        return;
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();
    writer.Key("tags");
    writer.StartArray();
    if (tag_list_) {
        for (const auto& tag : tag_list_()) {
            writer.StartObject();
            writer.Key("name");
            writer.String(tag.name.c_str());
            writer.Key("table");
            writer.String(tag.table.c_str());
            writer.Key("type");
            writer.String(tag.type.c_str());
            writer.Key("address");
            writer.String(tag.address.c_str());
            writer.EndObject();
        }
    }
    writer.EndArray();
    writer.EndObject();
    t_res.set_content(sb.GetString(), "application/json");
}

void HttpAdapter::handleGetTag(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    const std::string name = percentDecode(t_req.path);
    if (!tag_read_ || !tag_list_) {
        t_res.status = 404;
        t_res.set_content(R"({"error":"Tag access not available"})", "application/json");
        return;
    }
    bool known = false;
    for (const auto& tag : tag_list_()) {
        if (tag.name == name) {
            known = true;
            break;
        }
    }
    if (!known) {
        t_res.status = 404;
        t_res.set_content(fmt::format(R"({{"error":"Unknown tag '{}'"}})", name), "application/json");
        return;
    }
    // Tags live outside DB ACLs: the default policy applies.
    if (!isAuthorizedField(t_req, std::nullopt, name, false)) {
        t_res.status = 403;
        t_res.set_content(R"({"error":"Forbidden"})", "application/json");
        return;
    }
    auto res = tag_read_(name);
    if (res.hasError()) {
        t_res.status = 500;
        t_res.set_content(fmt::format(R"({{"error":{}}})", jsonQuote(res.error())), "application/json");
        return;
    }
    t_res.set_content(res.value(), "application/json");
}

void HttpAdapter::handleWriteTag(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    const std::string name = percentDecode(t_req.path);
    if (!tag_write_ || !tag_list_) {
        t_res.status = 404;
        t_res.set_content(R"({"error":"Tag access not available"})", "application/json");
        return;
    }
    bool known = false;
    for (const auto& tag : tag_list_()) {
        if (tag.name == name) {
            known = true;
            break;
        }
    }
    if (!known) {
        t_res.status = 404;
        t_res.set_content(fmt::format(R"({{"error":"Unknown tag '{}'"}})", name), "application/json");
        return;
    }
    if (!isAuthorizedField(t_req, std::nullopt, name, true)) {
        t_res.status = 403;
        t_res.set_content(R"({"error":"Forbidden"})", "application/json");
        return;
    }
    if (t_req.body.empty()) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"Empty body: expected a JSON value"})", "application/json");
        return;
    }
    // Accept raw scalars too (42.5, true) — normalize like tagPut does.
    const std::string json = ::sgrn::gateway::twin::parseRawValuePayload(t_req.body);
    auto res = tag_write_(name, json);
    if (res.hasError()) {
        t_res.status = 400;
        t_res.set_content(fmt::format(R"({{"error":{}}})", jsonQuote(res.error())), "application/json");
        return;
    }
    t_res.set_content(fmt::format(R"({{"ok":true,"name":{}}})", jsonQuote(name)), "application/json");
}

} // namespace sgrn::gateway::adapters
