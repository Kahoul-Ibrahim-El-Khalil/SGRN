#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/services/admin-repository.hpp>

namespace sgrn::datastore::services::admin
{
namespace
{
BackendError databaseError(const std::exception& t_error) {
    return makeBackendError(BackendErrorKind::Database, t_error.what());
}
} // namespace

drogon::Task<BackendResult<RoleAccess>> AdminRepository::roleAccess(int32_t t_role_id) {
    try {
        static constexpr char kQuery[] = "SELECT organisation, is_system AS system FROM core.roles WHERE id = $1";
        const auto rows = co_await db_->execSqlCoro(kQuery, t_role_id);
        CO_RETURN_ERROR_IF(rows.empty(), makeBackendError(BackendErrorKind::Auth, "role not found"));
        co_return sgrn::orm::fromRow<RoleAccess, kQuery>(rows[0]);
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<std::vector<RoleRecord>>> AdminRepository::listRoles(const std::string& t_organisation) {
    try {
        static constexpr char kQuery[] = "SELECT id, name, description, permissions, is_system AS system, created_at, updated_at "
                                         "FROM core.roles WHERE organisation = $1 ORDER BY name";
        const auto rows = co_await db_->execSqlCoro(kQuery, t_organisation);
        co_return sgrn::orm::fromResult<RoleRecord, kQuery>(rows);
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<int32_t>> AdminRepository::createRole(
    const std::string& t_organisation, const std::string& t_name, const std::string& t_description, const std::string& t_permissions) {
    try {
        const auto rows = co_await db_->execSqlCoro("INSERT INTO core.roles (organisation, name, description, permissions, is_system) "
                                                    "VALUES ($1, $2, $3, $4::jsonb, false) RETURNING id",
            t_organisation, t_name, t_description, t_permissions);
        CO_RETURN_ERROR_IF(rows.empty(), makeBackendError(BackendErrorKind::Database, "role insert returned no row"));
        co_return rows[0]["id"].as<int32_t>();
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<bool>> AdminRepository::updateRole(int32_t t_role_id, const std::string& t_organisation,
    const std::optional<std::string>& t_description, const std::optional<std::string>& t_permissions) {
    try {
        std::string sql = "UPDATE core.roles SET updated_at = NOW()";
        std::vector<std::string> params{t_organisation, std::to_string(t_role_id)};
        int index = 3;
        if (t_description.has_value()) {
            sql += ", description = $" + std::to_string(index++);
            params.push_back(*t_description);
        }
        if (t_permissions.has_value()) {
            sql += ", permissions = $" + std::to_string(index++) + "::jsonb";
            params.push_back(*t_permissions);
        }
        sql += " WHERE organisation = $1 AND id = $2 AND is_system = false";
        const auto result = co_await ::sgrn::datastore::core::execSqlCoroVec(db_, sql, params);
        co_return result.affectedRows() != 0;
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<bool>> AdminRepository::deleteRole(int32_t t_role_id, const std::string& t_organisation) {
    try {
        const auto result = co_await db_->execSqlCoro(
            "DELETE FROM core.roles WHERE organisation = $1 AND id = $2 AND is_system = false", t_organisation, t_role_id);
        co_return result.affectedRows() != 0;
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<SubjectOrganisation>> AdminRepository::userOrganisation(int32_t t_user_id) {
    try {
        const auto rows = co_await db_->execSqlCoro("SELECT organisation FROM core.users WHERE id = $1 AND deleted_at IS NULL", t_user_id);
        CO_RETURN_ERROR_IF(rows.empty(), makeBackendError(BackendErrorKind::Auth, "user not found"));
        co_return SubjectOrganisation{rows[0]["organisation"].as<std::string>()};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<SubjectOrganisation>> AdminRepository::serviceOrganisation(int32_t t_service_id) {
    try {
        const auto rows = co_await db_->execSqlCoro(
            "SELECT organisation FROM core.automated_services WHERE id = $1 AND deleted_at IS NULL", t_service_id);
        CO_RETURN_ERROR_IF(rows.empty(), makeBackendError(BackendErrorKind::Auth, "service not found"));
        co_return SubjectOrganisation{rows[0]["organisation"].as<std::string>()};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<bool>> AdminRepository::isLastPlatformAdmin(int32_t t_user_id) {
    try {
        const auto user =
            co_await db_->execSqlCoro("SELECT is_platform_admin FROM core.users WHERE id = $1 AND deleted_at IS NULL", t_user_id);
        if (user.empty() || !user[0]["is_platform_admin"].as<bool>())
            co_return false;
        const auto count =
            co_await db_->execSqlCoro("SELECT COUNT(*) AS count FROM core.users WHERE is_platform_admin = true AND deleted_at IS NULL");
        co_return !count.empty() && count[0]["count"].as<int64_t>() <= 1;
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<void>> AdminRepository::assignRole(const RoleTarget& t_target, int32_t t_role_id) {
    try {
        co_await db_->execSqlCoro("INSERT INTO core.user_roles (user_id, automated_service_id, role_id) VALUES ($1, $2, $3)",
            t_target.user_id, t_target.service_id, t_role_id);
        co_return BackendResult<void>{};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

drogon::Task<BackendResult<void>> AdminRepository::revokeRole(const RoleTarget& t_target, int32_t t_role_id) {
    try {
        if (t_target.user_id.has_value())
            co_await db_->execSqlCoro("DELETE FROM core.user_roles WHERE user_id = $1 AND role_id = $2", *t_target.user_id, t_role_id);
        else
            co_await db_->execSqlCoro(
                "DELETE FROM core.user_roles WHERE automated_service_id = $1 AND role_id = $2", *t_target.service_id, t_role_id);
        co_return BackendResult<void>{};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}
} // namespace sgrn::datastore::services::admin
