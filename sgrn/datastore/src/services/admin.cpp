#include <regex>
/* sgrn/api/src/admin/helpers.cpp */

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/services/admin.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/strings.hpp>
#include <json/json.h>
#include <memory>
#include <orm/models/core/Users.h>
#include <regex>
#include <vector>

#ifdef DEBUG_ADMIN_SERVICE
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("AdminService", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("AdminService", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("AdminService", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("AdminService", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::handlers::admin
{
using drogon::Task;
using namespace drogon::orm;
using namespace drogon_model::sgrn;

/* ---------------------------------------------------------
 * PURE DESERIALIZATION LOGIC
 * --------------------------------------------------------- */
std::optional<RegisterUserPayload> deserializeRegisterUserPayload(
    const std::shared_ptr<Json::Value>& tsp_json, std::string& t_out_error_message) {
    if (!tsp_json) {
        t_out_error_message = "Invalid JSON body";
        return std::nullopt;
    }

    RegisterUserPayload p;
    p.first_name = sgrn::utils::strings::toPascalCase(sgrn::utils::strings::trim(tsp_json->get("first_name", "").asString()));
    p.family_name = sgrn::utils::strings::toPascalCase(sgrn::utils::strings::trim(tsp_json->get("family_name", "").asString()));
    p.email = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(tsp_json->get("email", "").asString()));
    p.password = tsp_json->get("password", "").asString();
    p.phone_number = sgrn::utils::strings::trim(tsp_json->get("phone_number", "").asString());
    p.organisation = sgrn::utils::strings::trim(tsp_json->get("organisation", "").asString());

    p.status = sgrn::utils::strings::trim(tsp_json->get("status", "active").asString());
    p.domain = sgrn::utils::strings::trim(tsp_json->get("domain", "").asString());
    if (tsp_json->isMember("storage_limit") && (*tsp_json)["storage_limit"].isInt64()) {
        p.storage_limit = (*tsp_json)["storage_limit"].asInt64();
    }

    std::vector<std::string> missing_fields;
    if (p.first_name.empty())
        missing_fields.emplace_back("first_name");
    if (p.family_name.empty())
        missing_fields.emplace_back("family_name");
    if (p.email.empty())
        missing_fields.emplace_back("email");
    if (p.password.empty())
        missing_fields.emplace_back("password");
    if (p.organisation.empty())
        missing_fields.emplace_back("organisation");

    if (p.status.empty())
        missing_fields.emplace_back("status");

    if (!missing_fields.empty()) {
        t_out_error_message = "Missing required fields: ";
        for (size_t i = 0; i < missing_fields.size(); ++i) {
            t_out_error_message += missing_fields[i];
            if (i + 1 < missing_fields.size()) {
                t_out_error_message += ", ";
            }
        }
        return std::nullopt;
    }

    // ── Validation ───────────────────────────────────────────────────────────

    // 1. Length constraints
    if (p.first_name.length() > 64) {
        t_out_error_message = "First name is too long (max 64)";
        return std::nullopt;
    }
    if (p.family_name.length() > 64) {
        t_out_error_message = "Family name is too long (max 64)";
        return std::nullopt;
    }
    if (p.email.length() > 128) {
        t_out_error_message = "Email is too long (max 128)";
        return std::nullopt;
    }

    // 2. Email format
    static const std::regex kEmailRegex(R"(^[a-zA-Z0-9_.+-]+@[a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+$)");
    if (!std::regex_match(p.email, kEmailRegex)) {
        t_out_error_message = "Invalid email format";
        return std::nullopt;
    }

    // 3. Phone number (optional, but if present must be valid)
    if (!p.phone_number.empty()) {
        static const std::regex kPhoneRegex(R"(^\+?[0-9]{7,15}$)");
        if (!std::regex_match(p.phone_number, kPhoneRegex)) {
            t_out_error_message = "Invalid phone number format (7-15 digits)";
            return std::nullopt;
        }
    }

    return p;
}

/* ---------------------------------------------------------
 * USER REGISTRATION
 *
 * Schema change: core.roles table removed.
 * core.users.role is now a TEXT column with a CHECK constraint
 * (values: 'admin', 'user').  We pass the role string directly
 * to the ORM — no role-ID lookup needed.
 * --------------------------------------------------------- */
Task<Json::Value> registerUser(
    const drogon::orm::DbClientPtr& tsp_db_client, const RegisterUserPayload& t_register_user_payload, const std::string& t_role) {
    Json::Value response;
    std::string role_lower = sgrn::utils::strings::toLower(t_role);

    // Validate the role value before hitting the DB so we surface a clear
    // error instead of a cryptic CHECK-constraint violation.
    if (role_lower != "admin" && role_lower != "user") {
        ERROR_LOG("Invalid role provided: {}", role_lower);
        response["success"] = false;
        response["error"] = "Invalid role — must be 'admin' or 'user'";
        co_return response;
    }
    /* -----------------------------------------------------
     * Build and Persist user — using raw SQL to support new 'domain' column
     * while avoiding immediate ORM regeneration.
     * ----------------------------------------------------- */
    try {
        auto res = co_await tsp_db_client->execSqlCoro(
            "INSERT INTO core.users (first_name, family_name, email, password, role, organisation, status, domain, phone_number, "
            "storage_limit) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
            "RETURNING id, first_name, family_name, email, role, organisation, status, domain, phone_number",
            t_register_user_payload.first_name, t_register_user_payload.family_name, t_register_user_payload.email,
            t_register_user_payload.password, role_lower, t_register_user_payload.organisation, t_register_user_payload.status,
            t_register_user_payload.domain,
            t_register_user_payload.phone_number.empty() ? std::optional<std::string>{} : t_register_user_payload.phone_number,
            t_register_user_payload.storage_limit);

        if (res.empty()) {
            response["success"] = false;
            response["error"] = "Failed to insert user";
            co_return response;
        }

        const auto& row = res[0];
        Json::Value user_json;
        user_json["id"] = row["id"].as<int32_t>();
        user_json["first_name"] = row["first_name"].as<std::string>();
        user_json["family_name"] = row["family_name"].as<std::string>();
        user_json["email"] = row["email"].as<std::string>();
        user_json["role"] = row["role"].as<std::string>();
        user_json["organisation"] = row["organisation"].as<std::string>();

        user_json["status"] = row["status"].as<std::string>();
        user_json["domain"] = row["domain"].isNull() ? "" : row["domain"].as<std::string>();
        user_json["phone_number"] = row["phone_number"].isNull() ? "" : row["phone_number"].as<std::string>();

        response["success"] = true;
        response["message"] = "User registered successfully";
        response["user"] = std::move(user_json);

    } catch (const std::exception& e) {
        ERROR_LOG("Database error during user registration: {}", e.what());
        response["success"] = false;
        response["error"] = "Registration failed: Database error";
    }

    co_return response;
}

} // namespace sgrn::datastore::handlers::admin

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
