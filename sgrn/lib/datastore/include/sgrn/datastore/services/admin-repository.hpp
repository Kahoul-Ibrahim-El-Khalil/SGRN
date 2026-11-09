#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/BackendError.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::datastore::services::admin
{
struct SubjectOrganisation {
    std::string organisation;
};

struct RoleAccess {
    std::string organisation;
    bool system{};
};

struct RoleRecord {
    int32_t id{};
    std::string name;
    std::string description;
    std::string permissions;
    bool system{};
    std::string created_at;
    std::string updated_at;
};

struct RoleTarget {
    std::optional<int32_t> user_id;
    std::optional<int32_t> service_id;

    [[nodiscard]] bool isUser() const noexcept {
        return user_id.has_value();
    }
    [[nodiscard]] int32_t id() const noexcept {
        return user_id.value_or(service_id.value_or(0));
    }
};

class AdminRepository {
public:
    explicit AdminRepository(drogon::orm::DbClientPtr t_db)
        : db_(std::move(t_db)) {
    }

    drogon::Task<BackendResult<RoleAccess>> roleAccess(int32_t t_role_id);
    drogon::Task<BackendResult<std::vector<RoleRecord>>> listRoles(const std::string& t_organisation);
    drogon::Task<BackendResult<int32_t>> createRole(
        const std::string& t_organisation, const std::string& t_name, const std::string& t_description, const std::string& t_permissions);
    drogon::Task<BackendResult<bool>> updateRole(int32_t t_role_id, const std::string& t_organisation,
        const std::optional<std::string>& t_description, const std::optional<std::string>& t_permissions);
    drogon::Task<BackendResult<bool>> deleteRole(int32_t t_role_id, const std::string& t_organisation);
    drogon::Task<BackendResult<SubjectOrganisation>> userOrganisation(int32_t t_user_id);
    drogon::Task<BackendResult<SubjectOrganisation>> serviceOrganisation(int32_t t_service_id);
    drogon::Task<BackendResult<bool>> isLastPlatformAdmin(int32_t t_user_id);
    drogon::Task<BackendResult<void>> assignRole(const RoleTarget& t_target, int32_t t_role_id);
    drogon::Task<BackendResult<void>> revokeRole(const RoleTarget& t_target, int32_t t_role_id);

private:
    drogon::orm::DbClientPtr db_;
};
} // namespace sgrn::datastore::services::admin
