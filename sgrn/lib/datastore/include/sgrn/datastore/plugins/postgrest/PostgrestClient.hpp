#pragma once
#include <drogon/HttpClient.h>
#include <drogon/plugins/Plugin.h>
#include <json/json.h>
#include <memory>
#include <string>

namespace sgrn::datastore::plugins
{

/**
 * Drogon Plugin: PostgrestClientPlugin
 *
 * Holds configuration and an HttpClient for communicating with an
 * independently-running PostgREST instance. This plugin does NOT
 * manage the PostgREST process lifecycle — PostgREST should be
 * started externally (systemd, Docker, shell script, etc.) before
 * this application launches.
 */
class PostgrestClient : public drogon::Plugin<PostgrestClient> {
public:
    PostgrestClient() = default;
    void initAndStart(const Json::Value& t_config) override;
    void shutdown() override;

    // Returns the shared HttpClient pointed at PostgREST.
    // Use this in your controllers/filters to proxy requests.
    drogon::HttpClientPtr getClient() const {
        return client_;
    }

    const std::string& getJwtSecret() const {
        return jwt_secret_;
    }

    const std::string& getBaseUrl() const {
        return base_url_;
    }

    uint16_t getPort() const {
        return port_;
    }

    drogon::Task<drogon::HttpResponsePtr> sendRequest(drogon::HttpRequestPtr tsp_req);

private:
    static constexpr uint16_t kDefaultPort = 3000;
    static constexpr auto kDefaultHost = "127.0.0.1";

    uint16_t port_ = kDefaultPort;
    std::string host_;
    std::string base_url_;
    std::string jwt_secret_;
    drogon::HttpClientPtr client_;
};

} // namespace sgrn::datastore::plugins
