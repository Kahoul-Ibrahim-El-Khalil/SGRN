// generated/domains.gen.hpp (auto-generated from manifest)
#pragma once

#include <sgrn/crud/CrudViewHandler.hpp>
#include <array>
#include <vector>

namespace sgrn::crud::generated
{

class DomainsView : public sgrn::crud::CrudViewHandler<DomainsView> {
public:
    static inline const sgrn::crud::CrudPolicy kPolicy{
        .read_scope = sgrn::crud::ReadScope::Org,
        .write_scope = sgrn::crud::WriteScope::Org,
        .delete_scope = sgrn::crud::DeleteScope::Admin,
        .allowed_roles = {"admin", "user"},
        .soft_delete = false,
        .audit = true,
        .versioning = false,
        .batch_enabled = false,
        .max_limit = 500,
    };

    static inline const sgrn::crud::CrudViewSpec kSpec{.table_schema = "core",
        .table_name = "domains",
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
            sgrn::crud::CrudFieldSpec{.name = "name",
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
            sgrn::crud::CrudFieldSpec{.name = "description",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = false,
                .operators = sgrn::crud::Op::None,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
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

    DomainsView()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DomainsView::handleList, "/api/v1/domains", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(DomainsView::handleCreate, "/api/v1/domains", drogon::Post, "UserAuthFilter");
    ADD_METHOD_TO(DomainsView::handleGet, "/api/v1/domains/{id}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(DomainsView::handleUpdate, "/api/v1/domains/{id}", drogon::Patch, "UserAuthFilter");
    ADD_METHOD_TO(DomainsView::handleDelete, "/api/v1/domains/{id}", drogon::Delete, "UserAuthFilter");
    METHOD_LIST_END

private:
    static inline const std::vector<sgrn::crud::RouteConfig> kRoutes = {
        {"/api/v1/domains", drogon::Get, {"UserAuthFilter"}}, {"/api/v1/domains", drogon::Post, {"UserAuthFilter"}}};

    static inline const std::vector<sgrn::crud::ItemRouteConfig> kItemRoutes = {{"/api/v1/domains/{id}", drogon::Get, {"UserAuthFilter"}},
        {"/api/v1/domains/{id}", drogon::Patch, {"UserAuthFilter"}}, {"/api/v1/domains/{id}", drogon::Delete, {"UserAuthFilter"}}};
};

} // namespace sgrn::crud::generated
