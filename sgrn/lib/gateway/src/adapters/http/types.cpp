#include <sgrn/gateway/adapters/http/types.hpp>
#include <cctype>

namespace sgrn::gateway::adapters::http
{

bool headerNameEquals(const std::string& t_a, const std::string& t_b) {
    if (t_a.size() != t_b.size())
        return false;
    for (size_t i = 0; i < t_a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(t_a[i])) != std::tolower(static_cast<unsigned char>(t_b[i])))
            return false;
    }
    return true;
}

bool HttpRequest::has_header(const std::string& t_name) const {
    for (const auto& [k, v] : headers) {
        if (headerNameEquals(k, t_name))
            return true;
    }
    return false;
}

std::string HttpRequest::get_header_value(const std::string& t_name) const {
    for (const auto& [k, v] : headers) {
        if (headerNameEquals(k, t_name))
            return v;
    }
    return "";
}

bool HttpRequest::has_param(const std::string& t_name) const {
    return query.find(t_name) != query.end();
}

std::string HttpRequest::get_param_value(const std::string& t_name) const {
    auto it = query.find(t_name);
    return it != query.end() ? it->second : "";
}

std::vector<std::string> HttpRequest::headerNames() const {
    std::vector<std::string> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers)
        out.push_back(k);
    return out;
}

void HttpResponse::set_content(const std::string& t_body, const std::string& t_content_type) {
    body = t_body;
    content_type = t_content_type;
}

void HttpResponse::set_content(const char* tp_data, size_t t_len, const std::string& t_content_type) {
    body.assign(tp_data != nullptr ? tp_data : "", t_len);
    content_type = t_content_type;
}

void HttpResponse::set_header(std::string t_key, std::string t_value) {
    headers[std::move(t_key)] = std::move(t_value);
}

HttpRequest fromCrowRequest(const crow::request& t_req, std::string t_captured_path) {
    HttpRequest out;
    out.method = crow::method_name(t_req.method);
    out.path = std::move(t_captured_path);
    out.raw_target = t_req.raw_url;
    out.body = t_req.body;
    out.remote_ip = t_req.remote_ip_address;
    for (const auto& [k, v] : t_req.headers) {
        out.headers.emplace_back(k, v);
    }
    for (const auto& key : t_req.url_params.keys()) {
        if (out.query.find(key) != out.query.end())
            continue; // first value wins
        if (char* v = t_req.url_params.get(key))
            out.query.emplace(key, v);
    }
    return out;
}

void applyToCrowResponse(const HttpResponse& t_src, crow::response& t_dst) {
    t_dst.code = t_src.status;
    t_dst.body = t_src.body;
    for (const auto& [k, v] : t_src.headers)
        t_dst.set_header(k, v);
    // Content-Type last so an explicit handler header (none set one today,
    // but be safe) is not silently clobbered... actually set_content is the
    // single source of truth — it always wins.
    t_dst.set_header("Content-Type", t_src.content_type);
}

void applyCors(crow::response& t_res, const crow::request& t_req) {
    const std::string& origin = t_req.get_header_value("Origin");
    if (origin.empty()) {
        t_res.set_header("Access-Control-Allow-Origin", "*");
    } else {
        // Single-origin reflection (dev default); operators add an allowlist
        // via policy when needed — same contract as the old httplib handler.
        t_res.set_header("Access-Control-Allow-Origin", origin);
        t_res.set_header("Vary", "Origin");
    }
    t_res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    t_res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

} // namespace sgrn::gateway::adapters::http
