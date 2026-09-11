/*sgrn/auth/include/sgrn/api/auth/service.hpp*/
#pragma once
#include <drogon/drogon.h>
#include <optional>
#include <regex>

namespace sgrn::datastore::handlers::auth
{

drogon::Task<drogon::HttpResponsePtr> performSignInProcess(
    drogon::HttpRequestPtr tsp_http_req, std::string t_email, std::string t_password);

drogon::Task<drogon::HttpResponsePtr> performAutomatedServiceSignInProcess(
    drogon::HttpRequestPtr tsp_http_req, std::string t_token, std::string t_secret);

drogon::Task<std::optional<Json::Value>> getUserCache(std::string t_user_email);

drogon::Task<bool> storeUserSession(std::string t_user_email, std::string t_sgrn_token);

drogon::Task<bool> deleteUserSession(std::string t_user_email);

drogon::Task<drogon::HttpResponsePtr> updatePassword(
    drogon::HttpRequestPtr tsp_http_req, std::string t_email, std::string t_old_password, std::string t_new_password);

} // namespace sgrn::datastore::handlers::auth
