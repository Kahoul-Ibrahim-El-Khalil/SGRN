#pragma once

// HttpRequest / HttpResponse — tiny framework-neutral facades over the
// northbound HTTP wire format.
//
// The gateway's REST handlers used to speak cpp-httplib's Request/Response
// directly. They now speak these structs instead, so the handler code is
// decoupled from the server framework (Crow today). The only framework
// touch-point is the route layer in http.cpp, which translates
// crow::request <-> HttpRequest and HttpResponse <-> crow::response via
// fromCrowRequest() / applyToCrowResponse() below.
//
// Naming deliberately mirrors the httplib accessors the handlers were written
// against (has_param/get_param_value, has_header/get_header_value,
// set_content/set_header) to keep the migration diff reviewable.

#include <crow.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::gateway::adapters::http
{

/// Case-insensitive ASCII compare for header names.
bool headerNameEquals(const std::string& t_a, const std::string& t_b);

struct HttpRequest {
    std::string method;     ///< "GET" / "POST" / "PUT" / "OPTIONS" / ...
    std::string path;       ///< route-captured sub-path, no leading '/' (was req.matches[1])
    std::string raw_target; ///< full request target, e.g. "/data/DB1?verbose=1"
    std::string body;       ///< raw request body bytes
    std::string remote_ip;  ///< client IP for ACL checks (was req.remote_addr)
    /// Request headers as (name, value) pairs preserving wire case and order
    /// (same shape as the old httplib::Headers multimap).
    std::vector<std::pair<std::string, std::string>> headers;
    /// Query parameters: first value wins (mirrors old has_param/get_param_value).
    std::map<std::string, std::string> query;

    bool has_header(const std::string& t_name) const;
    /// Returns "" when the header is absent (mirrors httplib semantics).
    std::string get_header_value(const std::string& t_name) const;
    bool has_param(const std::string& t_name) const;
    /// Returns "" when the parameter is absent; check has_param() first.
    std::string get_param_value(const std::string& t_name) const;
    /// Header names in wire order (for SecurityManager::authorizeHttp).
    std::vector<std::string> headerNames() const;
};

struct HttpResponse {
    int status{200};
    std::map<std::string, std::string> headers;
    std::string body;
    std::string content_type{"text/plain"};

    void set_content(const std::string& t_body, const std::string& t_content_type);
    void set_content(const char* tp_data, size_t t_len, const std::string& t_content_type);
    void set_header(std::string t_key, std::string t_value);
};

/// Build an HttpRequest from a Crow request. t_captured_path is the route's
/// `<path>` capture ("" when the route has no wildcard).
HttpRequest fromCrowRequest(const crow::request& t_req, std::string t_captured_path = "");

/// Copy status/body/headers/content-type into a Crow response.
void applyToCrowResponse(const HttpResponse& t_src, crow::response& t_dst);

/// CORS headers, mirroring the old httplib pre-routing handler: reflect the
/// request Origin when present (Vary: Origin), fall back to "*" otherwise.
/// Applied to every response, including errors and the SPA fallback.
void applyCors(crow::response& t_res, const crow::request& t_req);

} // namespace sgrn::gateway::adapters::http
