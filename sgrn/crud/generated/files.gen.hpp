// generated/files.gen.hpp (auto-generated from manifest)
#pragma once

#include <sgrn/crud/CrudViewHandler.hpp>
#include <array>
#include <vector>

namespace sgrn::crud::generated
{

class FilesView : public sgrn::crud::CrudViewHandler<FilesView> {
public:
    static inline const sgrn::crud::CrudPolicy kPolicy{
        .read_scope = sgrn::crud::ReadScope::Org,
        .write_scope = sgrn::crud::WriteScope::Own,
        .delete_scope = sgrn::crud::DeleteScope::Own,
        .allowed_roles = {"admin", "user", "service_account"},
        .soft_delete = true,
        .audit = true,
        .versioning = false,
        .batch_enabled = false,
        .max_limit = 500,
    };

    static inline const sgrn::crud::CrudViewSpec kSpec{.table_schema = "storage",
        .table_name = "files",
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
            sgrn::crud::CrudFieldSpec{.name = "filename",
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
            sgrn::crud::CrudFieldSpec{.name = "mime_type",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 100, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "size_bytes",
                .type = sgrn::crud::FieldType::BigInt,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq | sgrn::crud::Op::Gt | sgrn::crud::Op::Gte | sgrn::crud::Op::Lt | sgrn::crud::Op::Lte,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 0, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "s3_key",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = false,
                .operators = sgrn::crud::Op::None,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 512, .unique = false, .enum_values = {}},
                .nullable = false,
                .default_val = "",
                .soft_delete_field = false,
                .version_field = false},
            sgrn::crud::CrudFieldSpec{.name = "checksum_sha256",
                .type = sgrn::crud::FieldType::Text,
                .readable = true,
                .writable = true,
                .filterable = true,
                .operators = sgrn::crud::Op::Eq,
                .validation = sgrn::crud::FieldValidation{.format = "", .maxlen = 64, .unique = false, .enum_values = {}},
                .nullable = true,
                .default_val = "",
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

    FilesView()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(FilesView::handleList, "/api/v1/storage/files", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(FilesView::handleCreate, "/api/v1/storage/files", drogon::Post, "UserAuthFilter");
    ADD_METHOD_TO(FilesView::handleGet, "/api/v1/storage/files/{id}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO(FilesView::handleUpdate, "/api/v1/storage/files/{id}", drogon::Patch, "UserAuthFilter");
    ADD_METHOD_TO(FilesView::handleDelete, "/api/v1/storage/files/{id}", drogon::Delete, "UserAuthFilter");
    METHOD_LIST_END

private:
    static inline const std::vector<sgrn::crud::RouteConfig> kRoutes = {
        {"/api/v1/storage/files", drogon::Get, {"UserAuthFilter"}}, {"/api/v1/storage/files", drogon::Post, {"UserAuthFilter"}}};

    static inline const std::vector<sgrn::crud::ItemRouteConfig> kItemRoutes = {
        {"/api/v1/storage/files/{id}", drogon::Get, {"UserAuthFilter"}}, {"/api/v1/storage/files/{id}", drogon::Patch, {"UserAuthFilter"}},
        {"/api/v1/storage/files/{id}", drogon::Delete, {"UserAuthFilter"}}};
};

} // namespace sgrn::crud::generated
