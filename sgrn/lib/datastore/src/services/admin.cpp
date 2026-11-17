#include <regex>
/* sgrn/api/src/admin/helpers.cpp */

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/core/orm-glaze.hpp>
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
using sgrn::utils::strings::toLower;
using sgrn::utils::strings::toPascalCase;
using sgrn::utils::strings::trim;

/* ---------------------------------------------------------
 * PURE DESERIALIZATION LOGIC
 * --------------------------------------------------------- */
bool validateRegisterUserPayload(const RegisterUserPayload& p, std::string& t_out_error_message) {
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
        return false;
    }

    // ── Validation ───────────────────────────────────────────────────────────

    // 1. Length constraints
    if (p.first_name.length() > 64) {
        t_out_error_message = "First name is too long (max 64)";
        return false;
    }
    if (p.family_name.length() > 64) {
        t_out_error_message = "Family name is too long (max 64)";
        return false;
    }
    if (p.email.length() > 128) {
        t_out_error_message = "Email is too long (max 128)";
        return false;
    }

    // 2. Email format
    static const std::regex kEmailRegex(R"(^[a-zA-Z0-9_.+-]+@[a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+$)");
    if (!std::regex_match(p.email, kEmailRegex)) {
        t_out_error_message = "Invalid email format";
        return false;
    }

    // 3. Phone number (optional, but if present must be valid)
    if (!p.phone_number.empty()) {
        static const std::regex kPhoneRegex(R"(^\+?[0-9]{7,15}$)");
        if (!std::regex_match(p.phone_number, kPhoneRegex)) {
            t_out_error_message = "Invalid phone number format (7-15 digits)";
            return false;
        }
    }

    return true;
}

/* ---------------------------------------------------------
 * USER REGISTRATION
 *
 * Schema change: core.roles table removed.
 * core.users.role is now a TEXT column with a CHECK constraint
 * (values: 'admin', 'user').  We pass the role string directly
 * to the ORM — no role-ID lookup needed.
 * --------------------------------------------------------- */
Task<Result<datastore::admin::RegisteredUser, BackendError>> registerUser(
    const drogon::orm::DbClientPtr& tsp_db_client, const RegisterUserPayload& t_register_user_payload, const std::string& t_role) {

    using User = ::sgrn::datastore::admin::RegisteredUser;
    const std::string role_lower = toLower(t_role);

    // Validate the role value before hitting the DB so we surface a clear
    // error instead of a cryptic CHECK-constraint violation.
    if (role_lower != "admin" && role_lower != "user") {
        co_return BackendError(BackendErrorKind::Auth, "Invalid role — must be 'admin' or 'user'");
    }

    static constexpr char kInsert[] =
        "INSERT INTO core.users (first_name, family_name, email, password, role, organisation, status, domain, phone_number, "
        "storage_limit) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
        "RETURNING id, first_name, family_name, email, role, organisation, status, domain, phone_number";

    try {
        std::optional<std::string> domain = t_register_user_payload.domain.empty()
                                                ? std::optional<std::string>{}
                                                : std::optional<std::string>(t_register_user_payload.domain);

        std::optional<std::string> phone_number =
            t_register_user_payload.phone_number.empty() ? std::optional<std::string>{} : t_register_user_payload.phone_number;

        auto res = co_await tsp_db_client->execSqlCoro(kInsert, t_register_user_payload.first_name, t_register_user_payload.family_name,
            t_register_user_payload.email, t_register_user_payload.password, role_lower, t_register_user_payload.organisation,
            t_register_user_payload.status, std::move(domain), std::move(phone_number), t_register_user_payload.storage_limit);

        CO_RETURN_IF(res.empty(), BackendError(BackendErrorKind::Database, "Failed to insert user"));

        co_return sgrn::orm::fromRowByName<User>(res[0]);
    } catch (const std::exception& e) {
        co_return BackendError(BackendErrorKind::Database, std::string("Registration failed: ") + e.what());
    }
}

} // namespace sgrn::datastore::handlers::admin

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
