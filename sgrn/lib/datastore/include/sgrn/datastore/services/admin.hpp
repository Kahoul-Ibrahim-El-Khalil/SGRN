/*sgrn/admin/include/sgrn/api/admin/helper.hpp*/
#pragma once
#include <drogon/orm/DbClient.h>
#include <cstdint>
#include <string>

#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/registerRoute.hpp>
#include <sgrn/debug.hpp>
#include <cstdint>
#include <regex>
#include <string>
namespace sgrn::datastore::handlers::admin
{
struct RegisterUserPayload {
    std::string first_name, family_name, email, password, phone_number;
    std::string organisation, status, domain;
    std::optional<int64_t> storage_limit;
};

std::optional<RegisterUserPayload> deserializeRegisterUserPayload(
    const std::shared_ptr<Json::Value>& tsp_json, std::string& t_out_error_message);

drogon::Task<Json::Value> registerUser(
    const drogon::orm::DbClientPtr& tsp_db_client, const RegisterUserPayload& t_register_user_payload, const std::string& t_role = "user");

} // namespace sgrn::datastore::handlers::admin
