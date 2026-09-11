#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/datastore/plugins/postgrest/PostgrestError.hpp>

#include <drogon/HttpTypes.h>
#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/jwt.hpp>

#ifdef DEBUG_PLUGIN_POSTGREST
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("PostgREST", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("PostgREST", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("PostgREST", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("PostgREST", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::plugins
{
using namespace drogon;

void PostgrestClient::initAndStart(const Json::Value& t_config) {
    port_ = static_cast<uint16_t>(t_config.get("port", kDefaultPort).asUInt());
    host_ = t_config.get("host", kDefaultHost).asString();
    jwt_secret_ = t_config.get("jwt_secret", "").asString();

    base_url_ = fmt::format("http://{}:{}", host_, port_);
    client_ = drogon::HttpClient::newHttpClient(base_url_);

    INFO_LOG("PostgrestClient Plugin initialized: {}", base_url_);
    if (jwt_secret_.empty()) {
        WARN_LOG("PostgrestClient Plugin: JWT secret is empty!");
    }
}

void PostgrestClient::shutdown() {
    INFO_LOG("PostgrestClient Plugin shutting down");
}

// Proxy request to PostgREST while preserving headers and method
// This allows the main app to handle auth/validation before forwarding to PostgREST
Task<HttpResponsePtr> PostgrestClient::sendRequest(HttpRequestPtr tsp_req) {
    try {
        // Create new request to PostgREST with same method and path
        auto t_req = HttpRequest::newHttpRequest();
        t_req->setMethod(t_req->getMethod());

        // Fix #2: Strip the proxy prefix so PostgREST receives the correct resource path
        static constexpr std::string_view kProxyPrefix = "/api/v1/postgrest";
        std::string path = std::string(t_req->getPath());
        if (path.starts_with(kProxyPrefix)) {
            path = path.substr(kProxyPrefix.length());
        }
        if (path.empty()) {
            path = "/";
        }

        t_req->setPath(path);

        // Fix #1: Forward all query parameters (critical for PostgREST filtering/selection)
        for (const auto& [key, val] : t_req->getParameters()) {
            t_req->setParameter(key, val);
        }

        // JWT Swap: Translate client-facing JWT to internal PostgREST JWT
        const std::string& auth_header = t_req->getHeader("Authorization");
        static constexpr std::string_view kBearerPrefix = "Bearer ";
        if (!auth_header.empty() && auth_header.starts_with(kBearerPrefix)) {
            std::string incoming_token = auth_header.substr(kBearerPrefix.length());
            const std::string& client_secret = drogon::app().getCustomConfig()["jwt_secret"].asString();

            auto claims_res = sgrn::utils::jwt::verifyToken(incoming_token, client_secret);
            if (claims_res) {
                // Generate new token for PostgREST using our internal secret
                Json::Value internal_claims = *claims_res;

                // Map internal role to database role
                std::string role = "sgrn_postgrest";

                // Check if user is admin
                if (internal_claims.isMember("user") && internal_claims["user"].isMember("role")) {
                    auto& role_val = internal_claims["user"]["role"];
                    if (role_val.isObject() && role_val.isMember("name")) {
                        if (role_val["name"].asString() == "admin") {
                            role = "sgrn_datastore";
                        }
                    } else if (role_val.isString() && role_val.asString() == "admin") {
                        role = "sgrn_datastore";
                    } else if (role_val.isInt() && role_val.asInt() == 0) { // Legacy check
                        role = "sgrn_datastore";
                    }
                }

                internal_claims["role"] = role;

                auto postgrest_token_res = sgrn::utils::jwt::generateToken(internal_claims, jwt_secret_);
                if (postgrest_token_res) {
                    t_req->addHeader("Authorization", "Bearer " + *postgrest_token_res);
                } else {
                    ERROR_LOG("Failed to generate PostgREST token: {}", postgrest_token_res.error());
                }
            }
        }

        // Forward request body if present
        if (tsp_req->getBody().length() > 0) {
            t_req->setBody(std::string(t_req->getBody()));
        }

        // Forward Content-Type if present
        auto content_type = t_req->getContentType();
        if (content_type != drogon::CT_NONE) {
            t_req->setContentTypeCode(content_type);
        }

        DEBUG_LOG("Forwarding {} request to PostgREST: {}", drogon::to_string(t_req->getMethod()), t_req->getPath());

        if (!client_) {
            co_return sgrn::createErrorResponse(
                ::sgrn::datastore::plugins::toBackendError(::sgrn::datastore::plugins::PostgrestError::NotInitialized),
                drogon::k500InternalServerError);
        }

        // Send request to PostgREST and await response
        auto resp = co_await client_->sendRequestCoro(t_req);

        DEBUG_LOG("PostgREST responded with status: {}", static_cast<int>(resp->getStatusCode()));

        // Return response as-is - the response already contains proper headers
        // from PostgREST (Content-Type, etc.)
        co_return resp;

    } catch (const std::exception& e) {
        ERROR_LOG("Error forwarding request to PostgREST: {}", e.what());
        co_return sgrn::createErrorResponse(
            ::sgrn::datastore::plugins::toBackendError(
                ::sgrn::datastore::plugins::PostgrestError::RequestFailed, "Failed to communicate with the database rest service"),
            drogon::k503ServiceUnavailable);
    }
}
} // namespace sgrn::datastore::plugins

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
