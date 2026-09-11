#pragma once

#include <drogon/drogon.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <filesystem>
#include <string>
namespace sgrn::datastore::config
{

constexpr const char system_wide_sgrn_config_json[] = "/etc/sgrn/sgrn.json";
constexpr const char user_sgrn_config_json[] = "~/.local/share/sgrn/sgrn.json";

using ::sgrn::utils::filesystem::expandUserPath;

inline std::string getConfigPath() {
    if (const char* p_env_path = std::getenv("SGRN_CONFIG_PATH")) {
        return expandUserPath(p_env_path);
    }
    if (std::filesystem::exists(system_wide_sgrn_config_json)) {
        return system_wide_sgrn_config_json;
    }
    return expandUserPath(user_sgrn_config_json);
}

inline ::sgrn::Result<void, std::string> configDrogonApp(int t_argc, char** tp_argv) {
    std::filesystem::path config_file;
    if (t_argc > 1) {
        config_file = std::filesystem::path(std::string(tp_argv[1]));
    } else {
        config_file = std::filesystem::path(getConfigPath());
    }
    if (!std::filesystem::exists(config_file)) {
        return "Config file not found: " + config_file.string();
    }
    drogon::app().loadConfigFile(config_file.string());

    drogon::app().setDefaultHandler(
        [](drogon::HttpRequestPtr tsp_http_req, std::function<void(drogon::HttpResponsePtr)>&& tsp_response_callback) {
            sgrn::respondWithError("Unkown path", drogon::k403Forbidden, tsp_response_callback);
        });
    return {};
}

} // namespace sgrn::datastore::config
