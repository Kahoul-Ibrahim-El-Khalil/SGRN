// generated/user_domain_permissions.gen.hpp (auto-generated from manifest)
#pragma once

#include <sgrn/crud/CrudViewHandler.hpp>
#include <array>
#include <vector>

namespace sgrn::crud::generated
{

class UserDomainPermissionsView : public sgrn::crud::CrudViewHandler<UserDomainPermissionsView> {
public:
    static inline const sgrn::crud::CrudPolicy kPolicy{
        .read_scope = sgrn::crud::ReadScope::Org,
        .write_scope = sgrn::crud::WriteScope::Admin,
        .delete_scope = sgrn::crud::DeleteScope::Admin,
        .allowed_roles = {"admin", "user"},
        .soft_delete = false,
        .audit = true,
        .versioning = false,
        .batch_enabled = false,
        .max_limit = 500,
    };

    static inline const sgrn::crud::CrudViewSpec kSpec{.table_schema = "core",
        .table_name = "user_domain_permissions",
        .primary_key = "id",
        .primary_key_type = "int",
        .tenant_column = "organisation",
        .policy = kPolicy,
        .fields = {sgrn::crud::CrudFieldSpec{.name = "id",
                       .type = sgrn::crud::FieldType::Int,
                       .readable = false,
                       .writable = false,
                       .filterable = false,
                       .operators = sgrn::crud::Op::None,
                       .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                       .nullable = true,
                       .default_val = "",
                       .soft_delete_field = false,
                       .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "organisation",
                .type = sgrn::crud::FieldType::Text,
                .readable = false,
                .writable = false,
                .filterable = false,
                .operators = sgrn::crud::Op::None,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "user_id",
                .type = sgrn::crud::FieldType::Int,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::In,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "domain",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Like,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 255, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "permission",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "read",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "created_at",
                .type = sgrn::crud::FieldType::Timestamp,
                .readable = true,
                .writable = false,
                .filterable = false,
                .operators = sgrn::crud::Op::None,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false}},
        .default_order = "id DESC",
        .admin_overrides = {}};

    UserDomainPermissionsView()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserDomainPermissionsView::handleList, "/api/v1/user-domain-permissions", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(UserDomainPermissionsView::handleCreate, "/api/v1/user-domain-permissions", drogon::Post, "UserAuthFilter");
    ADD_METHOD_TO(UserDomainPermissionsView::handleGet, "/api/v1/user-domain-permissions/{id}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(UserDomainPermissionsView::handleUpdate, "/api/v1/user-domain-permissions/{id}", drogon::Patch, "UserAuthFilter");
    ADD_METHOD_TO(UserDomainPermissionsView::handleDelete, "/api/v1/user-domain-permissions/{id}", drogon::Delete, "UserAuthFilter");
    METHOD_LIST_END

private:
    static inline const std::vector<sgrn::crud::RouteConfig> kRoutes = {
        {"/api/v1/user-domain-permissions", drogon::Get, {"UserAuthFilter"}},
        {"/api/v1/user-domain-permissions", drogon::Post, {"UserAuthFilter"}}};

    static inline const std::vector<sgrn::crud::ItemRouteConfig> kItemRoutes = {
        {"/api/v1/user-domain-permissions/{id}", drogon::Get, {"UserAuthFilter"}},
        {"/api/v1/user-domain-permissions/{id}", drogon::Patch, {"UserAuthFilter"}},
        {"/api/v1/user-domain-permissions/{id}", drogon::Delete, {"UserAuthFilter"}}};
};

} // namespace sgrn::crud::generated
