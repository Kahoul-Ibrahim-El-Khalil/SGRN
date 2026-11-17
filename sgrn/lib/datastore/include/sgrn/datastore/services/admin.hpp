/*sgrn/admin/include/sgrn/api/admin/helper.hpp*/
#pragma once
#include <drogon/orm/DbClient.h>
#include <cstdint>
#include <string>

#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/registerRoute.hpp>
#include <sgrn/datastore/wire/admin.hpp>
#include <sgrn/debug.hpp>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
namespace sgrn::datastore::handlers::admin
{
struct RegisterUserPayload {
    std::string first_name, family_name, email, password, phone_number;
    std::string organisation, status, domain;
    std::optional<int64_t> storage_limit;
};

bool validateRegisterUserPayload(const RegisterUserPayload& p, std::string& t_out_error_message);

/// Insert a user. Returns the created row as a typed value: the wire type is
/// the domain type, so the handler neither builds nor inspects JSON.
drogon::Task<Result<::sgrn::datastore::admin::RegisteredUser, BackendError>> registerUser(
    const drogon::orm::DbClientPtr& tsp_db_client, const RegisterUserPayload& t_register_user_payload, const std::string& t_role = "user");

} // namespace sgrn::datastore::handlers::admin
