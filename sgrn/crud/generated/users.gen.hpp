// generated/users.gen.hpp (auto-generated from manifest)
#pragma once

#include <sgrn/crud/CrudViewHandler.hpp>
#include <array>
#include <vector>

namespace sgrn::crud::generated
{

class UsersView : public sgrn::crud::CrudViewHandler<UsersView> {
public:
    static inline const sgrn::crud::CrudPolicy kPolicy{
        .read_scope = sgrn::crud::ReadScope::Org,
        .write_scope = sgrn::crud::WriteScope::Own,
        .delete_scope = sgrn::crud::DeleteScope::Admin,
        .allowed_roles = {"admin", "user", "service_account"},
        .soft_delete = true,
        .audit = true,
        .versioning = true,
        .batch_enabled = false,
        .max_limit = 500,
    };

    static inline const sgrn::crud::CrudViewSpec kSpec{.table_schema = "core",
        .table_name = "users",
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
            sgrn::crud::CrudFieldSpec{.name = "email",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Like | sgrn::crud::Op::In,
                .validation = sgrn::crud::FieldValidation{.format = "email", .maxlen = 255, .unique = true, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "first_name",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Like,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 100, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "family_name",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Like,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 100, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "role",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "user",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "is_active",
                .type = sgrn::crud::FieldType::Bool,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "True",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "deleted_at",
                .type = sgrn::crud::FieldType::Timestamp,
                .readable = false,
                .writable = false,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Neq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
                .soft_delete_field = true,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "version",
                .type = sgrn::crud::FieldType::Int,
                .readable = false,
                .writable = false,
                .filterable = false,
                .operators = sgrn::crud::Op::None,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = true},
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
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "updated_at",
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

    UsersView()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UsersView::handleList, "/api/v1/users", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(UsersView::handleCreate, "/api/v1/users", drogon::Post, "UserAuthFilter");
    ADD_METHOD_TO(UsersView::handleGet, "/api/v1/users/{id}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(UsersView::handleUpdate, "/api/v1/users/{id}", drogon::Patch, "UserAuthFilter");
    ADD_METHOD_TO(UsersView::handleDelete, "/api/v1/users/{id}", drogon::Delete, "UserAuthFilter");
    METHOD_LIST_END

private:
    static inline const std::vector<sgrn::crud::RouteConfig> kRoutes = {
        {"/api/v1/users", drogon::Get, {"UserAuthFilter"}}, {"/api/v1/users", drogon::Post, {"UserAuthFilter"}}};

    static inline const std::vector<sgrn::crud::ItemRouteConfig> kItemRoutes = {{"/api/v1/users/{id}", drogon::Get, {"UserAuthFilter"}},
        {"/api/v1/users/{id}", drogon::Patch, {"UserAuthFilter"}}, {"/api/v1/users/{id}", drogon::Delete, {"UserAuthFilter"}}};
};

} // namespace sgrn::crud::generated
