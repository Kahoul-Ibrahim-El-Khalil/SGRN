#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/services/admin-repository.hpp>

namespace sgrn::datastore::services::admin
{
namespace
{
using drogon::Task;

struct RoleIdRow {
    int32_t id{0};
};

struct PlatformAdminRow {
    bool is_platform_admin{false};
};

struct UserCountRow {
    int64_t count{0};
};

BackendError databaseError(const std::exception& t_error) {
    return BackendError(BackendErrorKind::Database, t_error.what());
}
} // namespace

Task<Result<RoleAccess, BackendError>> AdminRepository::roleAccess(int32_t t_role_id) {
    try {
        static constexpr char kQuery[] = "SELECT organisation, is_system AS system FROM core.roles WHERE id = $1";

        const auto rows = co_await db_->execSqlCoro(kQuery, t_role_id);

        CO_RETURN_ERROR_IF(rows.empty(), BackendError(BackendErrorKind::Auth, "role not found"));

        co_return sgrn::orm::fromRow<RoleAccess, kQuery>(rows[0]);
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<std::vector<RoleRecord>, BackendError>> AdminRepository::listRoles(const std::string& t_organisation) {
    try {
        static constexpr char kQuery[] = "SELECT id, name, description, permissions, is_system AS system, created_at, updated_at "
                                         "FROM core.roles WHERE organisation = $1 ORDER BY name";
        const auto rows = co_await db_->execSqlCoro(kQuery, t_organisation);
        co_return sgrn::orm::fromResult<RoleRecord, kQuery>(rows);
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<int32_t, BackendError>> AdminRepository::createRole(
    const std::string& t_organisation, const std::string& t_name, const std::string& t_description, const std::string& t_permissions) {
    try {
        static constexpr char kQuery[] = "INSERT INTO core.roles (organisation, name, description, permissions, is_system) "
                                         "VALUES ($1, $2, $3, $4::jsonb, false) RETURNING id";
        const auto rows = co_await db_->execSqlCoro(kQuery, t_organisation, t_name, t_description, t_permissions);

        CO_RETURN_ERROR_IF(rows.empty(), BackendError(BackendErrorKind::Database, "role insert returned no row"));

        co_return sgrn::orm::fromRowByName<RoleIdRow>(rows[0]).id;

    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<bool, BackendError>> AdminRepository::updateRole(int32_t t_role_id, const std::string& t_organisation,
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

Task<Result<bool, BackendError>> AdminRepository::deleteRole(int32_t t_role_id, const std::string& t_organisation) {
    try {
        const auto result = co_await db_->execSqlCoro(
            "DELETE FROM core.roles WHERE organisation = $1 AND id = $2 AND is_system = false", t_organisation, t_role_id);

        co_return result.affectedRows() != 0;

    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<SubjectOrganisation, BackendError>> AdminRepository::userOrganisation(int32_t t_user_id) {
    try {

        static constexpr char kQuery[] = "SELECT organisation FROM core.users WHERE id = $1 AND deleted_at IS NULL";

        const auto rows = co_await db_->execSqlCoro(kQuery, t_user_id);

        CO_RETURN_ERROR_IF(rows.empty(), BackendError(BackendErrorKind::Auth, "user not found"));

        co_return sgrn::orm::fromRow<SubjectOrganisation, kQuery>(rows[0]);

    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<SubjectOrganisation, BackendError>> AdminRepository::serviceOrganisation(int32_t t_service_id) {
    try {
        static constexpr char kQuery[] = "SELECT organisation FROM core.automated_services WHERE id = $1 AND deleted_at IS NULL";

        const auto rows = co_await db_->execSqlCoro(kQuery, t_service_id);

        CO_RETURN_ERROR_IF(rows.empty(), BackendError(BackendErrorKind::Auth, "service not found"));

        co_return sgrn::orm::fromRow<SubjectOrganisation, kQuery>(rows[0]);

    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<bool, BackendError>> AdminRepository::isLastPlatformAdmin(int32_t t_user_id) {
    try {
        static constexpr char kUserQuery[] = "SELECT is_platform_admin FROM core.users WHERE id = $1 AND deleted_at IS NULL";
        static constexpr char kCountQuery[] =
            "SELECT COUNT(*) AS count FROM core.users WHERE is_platform_admin = true AND deleted_at IS NULL";
        const auto user = co_await db_->execSqlCoro(kUserQuery, t_user_id);
        if (user.empty() || !sgrn::orm::fromRow<PlatformAdminRow, kUserQuery>(user[0]).is_platform_admin)
            co_return false;
        const auto count = co_await db_->execSqlCoro(kCountQuery);
        co_return !count.empty() && sgrn::orm::fromRow<UserCountRow, kCountQuery>(count[0]).count <= 1;
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<void, BackendError>> AdminRepository::assignRole(const RoleTarget& t_target, int32_t t_role_id) {
    try {
        co_await db_->execSqlCoro("INSERT INTO core.user_roles (user_id, automated_service_id, role_id) VALUES ($1, $2, $3)",
            t_target.user_id, t_target.service_id, t_role_id);
        co_return Result<void, BackendError>{};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}

Task<Result<void, BackendError>> AdminRepository::revokeRole(const RoleTarget& t_target, int32_t t_role_id) {
    try {
        if (t_target.user_id.has_value())
            co_await db_->execSqlCoro("DELETE FROM core.user_roles WHERE user_id = $1 AND role_id = $2", *t_target.user_id, t_role_id);
        else
            co_await db_->execSqlCoro(
                "DELETE FROM core.user_roles WHERE automated_service_id = $1 AND role_id = $2", *t_target.service_id, t_role_id);
        co_return {};
    } catch (const std::exception& e) {
        co_return databaseError(e);
    }
}
} // namespace sgrn::datastore::services::admin
