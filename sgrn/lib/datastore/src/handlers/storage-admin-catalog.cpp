#include <sgrn/datastore/handlers/storage-admin.hpp>

#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/init/rate-limit.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/utils/system-config.hpp>
#include <sgrn/datastore/wire/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage-admin-internal.hpp"

namespace sgrn::datastore::handlers::storage_admin
{
using namespace drogon;
using namespace detail;

// ============================================================================
// Formats registry + quota ledger
// ============================================================================

namespace
{

inline std::string lowerAscii(std::string t_in) {
    return sgrn::utils::strings::toLower(std::move(t_in));
}

[[maybe_unused]] inline bool jsonBool(const Json::Value& t_v, bool t_dflt) {
    return t_v.isBool() ? t_v.asBool() : t_dflt;
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleListFormats(HttpRequestPtr tsp_req) {
    try {
        const std::string search = lowerAscii(queryParam(tsp_req, "search", ""));
        const std::string allowed = lowerAscii(queryParam(tsp_req, "allowed", ""));
        if (!allowed.empty() && allowed != "true" && allowed != "false") {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"allowed must be true or false", k400BadRequest, "StorageAdmin"});
        }
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 500, 5000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        static constexpr char kQuery[] =
            "SELECT extension, mime_type, is_compressed, is_allowed, description, created_at FROM storage.formats";

        // $1 search (empty matches all), $2 allowed filter (empty = any).
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
            std::string(kQuery) +
                " WHERE (extension LIKE '%' || $1 || '%' ESCAPE '\\' OR mime_type ILIKE '%' || $1 || '%' ESCAPE '\\') "
                "AND ($2 = '' OR is_allowed = ($2 = 'true')) "
                "ORDER BY extension LIMIT " +
                std::to_string(limit),
            {escapeLike(search), allowed});

        co_return sgrn::createTypedResponse(tsp_req,
            FormatsResponse{
                .formats = sgrn::orm::fromResult<FormatRecord, kQuery>(rows),
                .count = static_cast<int64_t>(rows.size()),
            },
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpsertFormat(HttpRequestPtr tsp_req) {
    try {
        auto req_res = parseTypedRequest<detail::UpsertFormatRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Expected JSON body", k400BadRequest, "StorageAdmin"});
        }
        const auto& req = req_res.value();
        if (req.extension.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"extension (string) is required", k400BadRequest, "StorageAdmin"});
        }
        if (req.mime_type.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"mime_type (string) is required", k400BadRequest, "StorageAdmin"});
        }
        const std::string ext = lowerAscii(req.extension);
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }
        const std::string mime = req.mime_type;
        if (const std::string err = sgrn::datastore::sysconfig::validateMimeType(mime); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }
        const bool is_compressed = req.is_compressed;
        const bool is_allowed = req.is_allowed;
        const bool has_description = req.description.has_value();
        const std::string description = has_description ? *req.description : "";

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // Read-then-write so an upsert without `description` keeps the stored
        // text instead of blanking it — unlike a blind ON CONFLICT overwrite.
        auto existing =
            co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT extension FROM storage.formats WHERE extension = $1", {ext});
        FormatRecord row;
        bool created = false;
        // NB: the bind lists are hoisted into named variables. Building a
        // std::vector<std::string> from an initializer list directly inside a
        // co_await expression crashes GCC 14 (ICE in gimple_add_tmp_var).
        const std::vector<std::string> format_binds{
            ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false", description};
        if (existing.empty()) {
            auto inserted = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "INSERT INTO storage.formats (extension, mime_type, is_compressed, is_allowed, description) "
                "VALUES ($1, $2, ($3 = 'true'), ($4 = 'true'), NULLIF($5, '')) "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                format_binds);
            row = sgrn::orm::fromRow<FormatRecord>(inserted[0]);
            created = true;
        } else {
            // Only touch description when the caller sent one.
            const std::vector<std::string> update_binds{ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false",
                description, has_description ? "true" : "false"};
            auto updated = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "UPDATE storage.formats SET mime_type = $2, is_compressed = ($3 = 'true'), is_allowed = ($4 = 'true'), "
                "description = CASE WHEN $6 = 'true' THEN NULLIF($5, '') ELSE description END "
                "WHERE extension = $1 "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                update_binds);
            row = sgrn::orm::fromRow<FormatRecord>(updated[0]);
        }
        co_return sgrn::createTypedResponse(tsp_req,
            FormatUpsertResponse{
                row.extension,
                row.mime_type,
                row.is_compressed,
                row.is_allowed,
                std::move(row.description),
                row.created_at,
                created,
            },
            k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleDeleteFormat(HttpRequestPtr tsp_req) {
    try {
        const std::string ext = lowerAscii(queryParam(tsp_req, "extension", ""));
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // The FK from storage.files has no ON DELETE action: deleting a
        // referenced format would fail deep in Postgres. Refuse loudly with
        // the blast radius instead (toggle is_allowed=false to block new
        // uploads while history stays intact).
        auto refs = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM storage.files WHERE extension = $1", {ext});
        const long long referencing = refs.empty() ? 0 : fieldInt(refs[0][0]);
        if (referencing > 0) {
            co_return sgrn::createTypedResponse(tsp_req,
                FormatConflictResponse{
                    ext,
                    referencing,
                    "set is_allowed=false instead — history stays intact, new uploads are blocked",
                },
                k409Conflict);
        }

        auto deleted = co_await sgrn::datastore::core::execSqlCoroVec(db, "DELETE FROM storage.formats WHERE extension = $1", {ext});
        if (deleted.affectedRows() == 0) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Unknown format: " + ext, k404NotFound, "StorageAdmin"});
        }
        co_return sgrn::createTypedResponse(tsp_req, FormatDeleteResponse{ext}, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleListQuotas(HttpRequestPtr tsp_req) {
    try {
        const std::string kind = lowerAscii(queryParam(tsp_req, "kind", "user"));
        if (kind != "user" && kind != "service" && kind != "organisation" && kind != "domain") {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind must be user, service, organisation or domain", k400BadRequest, "StorageAdmin"});
        }
        const std::string search = queryParam(tsp_req, "search", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::string quota_sql;
        std::vector<std::string> quota_binds = {escapeLike(search)};
        if (kind == "user") {
            quota_sql = "SELECT id, email, organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, "
                        "rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
                        "FROM core.users WHERE deleted_at IS NULL AND (email ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY id LIMIT " + std::to_string(limit);
        } else if (kind == "service") {
            quota_sql = "SELECT id, name, organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, "
                        "rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
                        "FROM core.automated_services WHERE deleted_at IS NULL AND (name ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY id LIMIT " + std::to_string(limit);
        } else if (kind == "domain") {
            // Namespace caps: usage is the whole subtree, computed by
            // storage.domain_usage (files + directories, virtual bytes).
            quota_sql = "SELECT d.id, d.name, d.path, d.organisation, d.storage_limit AS storage_limit_bytes, "
                        "d.entry_count_limit, coalesce(u.virtual_size, 0) AS storage_used_bytes, "
                        "coalesce(u.entry_count, 0) AS entries_used "
                        "FROM core.domains d LEFT JOIN LATERAL storage.domain_usage(d.id) u ON true "
                        "WHERE (d.name ILIKE '%' || $1 || '%' ESCAPE '\\' OR d.path ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR d.organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND d.organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY d.organisation, d.path LIMIT " + std::to_string(limit);
        } else {
            quota_sql = "SELECT name, name AS organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, rate_limit_rpm "
                        "FROM core.organisations WHERE (name ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND name = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY name LIMIT " + std::to_string(limit);
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, quota_binds);

        std::vector<UserQuotaRow> users;
        std::vector<ServiceQuotaRow> services;
        std::vector<DomainQuotaRow> domains;
        std::vector<OrganisationQuotaRow> organisations;
        if (kind == "user") {
            users = sgrn::orm::fromResultByName<UserQuotaRow>(rows);
            for (auto& row : users) {
                row.kind = kind;
            }
        } else if (kind == "service") {
            services = sgrn::orm::fromResultByName<ServiceQuotaRow>(rows);
            for (auto& row : services) {
                row.kind = kind;
            }
        } else if (kind == "domain") {
            domains = sgrn::orm::fromResultByName<DomainQuotaRow>(rows);
            for (auto& row : domains) {
                row.kind = kind;
            }
        } else {
            organisations = sgrn::orm::fromResultByName<OrganisationQuotaRow>(rows);
            for (auto& row : organisations) {
                row.kind = kind;
            }
        }
        if (kind == "user") {
            co_return sgrn::createTypedResponse(
                tsp_req, UserQuotasResponse{kind, std::move(users), static_cast<int64_t>(rows.size())}, k200OK);
        }
        if (kind == "service") {
            co_return sgrn::createTypedResponse(
                tsp_req, ServiceQuotasResponse{kind, std::move(services), static_cast<int64_t>(rows.size())}, k200OK);
        }
        if (kind == "domain") {
            co_return sgrn::createTypedResponse(
                tsp_req, DomainQuotasResponse{kind, std::move(domains), static_cast<int64_t>(rows.size())}, k200OK);
        }
        co_return sgrn::createTypedResponse(
            tsp_req, OrganisationQuotasResponse{kind, std::move(organisations), static_cast<int64_t>(rows.size())}, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateQuota(HttpRequestPtr tsp_req) {
    try {
        auto req_res = parseTypedRequest<detail::StorageAdminUpdateQuotaRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Expected JSON body", k400BadRequest, "StorageAdmin"});
        }
        const auto& req = req_res.value();
        if (req.kind.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind (user|service|organisation) is required", k400BadRequest, "StorageAdmin"});
        }
        const std::string kind = lowerAscii(req.kind);
        if (kind != "user" && kind != "service" && kind != "organisation" && kind != "domain") {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind must be user, service, organisation or domain", k400BadRequest, "StorageAdmin"});
        }
        const bool has_storage = req.storage_limit_bytes.has_value();
        const bool has_entries = req.entry_count_limit.has_value();
        if (!has_storage && !has_entries) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{
                "supply storage_limit_bytes and/or entry_count_limit (null lifts the cap)", k400BadRequest, "StorageAdmin"});
        }
        const bool storage_is_null = has_storage && !req.storage_limit_bytes->has_value();
        const bool entries_is_null = has_entries && !req.entry_count_limit->has_value();
        if (has_storage && !storage_is_null && **req.storage_limit_bytes < 0) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"storage_limit_bytes must be a non-negative integer or null", k400BadRequest, "StorageAdmin"});
        }
        if (has_entries && !entries_is_null && **req.entry_count_limit < 0) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"entry_count_limit must be a non-negative integer or null", k400BadRequest, "StorageAdmin"});
        }

        std::string sets;
        std::vector<std::string> binds;
        auto push_set = [&](const char* t_col, bool t_is_null, const std::string& t_bind) {
            if (!sets.empty()) {
                sets += ", ";
            }
            if (t_is_null) {
                sets += std::string(t_col) + " = NULL";
            } else {
                binds.push_back(t_bind);
                sets += std::string(t_col) + " = $" + std::to_string(binds.size()) + "::bigint";
            }
        };
        if (has_storage) {
            push_set("storage_limit", storage_is_null, storage_is_null ? "" : std::to_string(**req.storage_limit_bytes));
        }
        if (has_entries) {
            push_set("entry_count_limit", entries_is_null, entries_is_null ? "" : std::to_string(**req.entry_count_limit));
        }

        // Deliberately no self-exclusion: an admin tunes their own quota
        // through the same call as anyone else's.
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        if (kind == "organisation") {
            if (!req.name.has_value() || req.name->empty()) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"name is required for kind organisation", k400BadRequest, "StorageAdmin"});
            }
            const std::string& target_org = *req.name;
            if (!is_platform && target_org != session_org) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"Forbidden: cannot modify quota of another organisation", k403Forbidden, "StorageAdmin"});
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::string quota_sql;
        if (kind == "user" || kind == "service") {
            if (!req.id.has_value() || *req.id <= 0) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"id (positive integer) is required for kind user|service", k400BadRequest, "StorageAdmin"});
            }
            binds.push_back(std::to_string(*req.id));
            const std::string table = kind == "user" ? "core.users" : "core.automated_services";
            const std::string idcol = kind == "user" ? "email" : "name";
            quota_sql = "UPDATE " + table + " SET " + sets + " WHERE id = $" + std::to_string(binds.size()) + " AND deleted_at IS NULL ";
            if (!is_platform) {
                binds.push_back(session_org);
                quota_sql += "AND organisation = $" + std::to_string(binds.size()) + " ";
            }
            quota_sql += "RETURNING id, " + idcol +
                         " AS label, organisation, total_virtual_size AS storage_used_bytes, "
                         "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit";
        } else if (kind == "domain") {
            if (!req.id.has_value() || *req.id <= 0) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"id (positive integer) is required for kind domain", k400BadRequest, "StorageAdmin"});
            }
            binds.push_back(std::to_string(*req.id));
            quota_sql = "UPDATE core.domains SET " + sets + " WHERE id = $" + std::to_string(binds.size()) + " ";
            if (!is_platform) {
                binds.push_back(session_org);
                quota_sql += "AND organisation = $" + std::to_string(binds.size()) + " ";
            }
            quota_sql += "RETURNING id, name AS label, organisation, "
                         "(SELECT u.virtual_size FROM storage.domain_usage(id) u) AS storage_used_bytes, "
                         "storage_limit AS storage_limit_bytes, "
                         "(SELECT u.entry_count FROM storage.domain_usage(id) u) AS entries_used, entry_count_limit";
        } else {
            const std::string target_org = *req.name;
            binds.push_back(target_org);
            quota_sql = "UPDATE core.organisations SET " + sets + " WHERE name = $" + std::to_string(binds.size()) +
                        " RETURNING NULL::bigint AS id, name AS label, name AS organisation, "
                        "total_virtual_size AS storage_used_bytes, storage_limit AS storage_limit_bytes, "
                        "total_entry_count AS entries_used, entry_count_limit";
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, binds);
        if (rows.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"No such " + kind + " (or it is soft-deleted)", k404NotFound, "StorageAdmin"});
        }
        const auto row = sgrn::orm::fromRowByName<QuotaUpdateRow>(rows[0]);

        // WP4c: this handler previously wrote no audit row at all (F7).
        // Details are the applied caps only — never request-echoed blobs.
        Json::Value applied(Json::objectValue);
        applied["storage_limit"] =
            row.storage_limit_bytes.has_value() ? Json::Value(Json::Int64(*row.storage_limit_bytes)) : Json::Value(Json::nullValue);
        applied["entry_count_limit"] =
            row.entry_count_limit.has_value() ? Json::Value(Json::Int64(*row.entry_count_limit)) : Json::Value(Json::nullValue);
        std::string user_org;
        int32_t user_id = 0;
        std::string user_email;
        bool has_user = false;
        if (tsp_req->attributes()->find("auth_context")) {
            const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
            user_org = auth.user.organisation;
            user_id = auth.user.id;
            user_email = auth.user.email;
            has_user = (user_id > 0);
        } else if (tsp_req->attributes()->find("session_claims")) {
            const auto& user = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
            user_org = user.organisation;
            user_id = user.id;
            user_email = user.email;
            has_user = (user_id > 0);
        }
        if (has_user) {
            std::string target_id = row.label;
            if (kind != "organisation" && kind != "domain") {
                target_id = std::to_string(row.id.value());
            }
            co_await sgrn::datastore::audit::AuditLogger::log(db, user_org, "user", user_id, user_email, "quota.updated", kind, target_id,
                tsp_req->getPeerAddr().toIp(), applied, "success");
        }
        if (kind == "organisation") {
            co_return sgrn::createTypedResponse(tsp_req,
                QuotaUpdateOrganisationResponse{
                    kind,
                    row.label,
                    row.organisation,
                    row.storage_used_bytes,
                    row.storage_limit_bytes,
                    row.entries_used,
                    row.entry_count_limit,
                },
                k200OK);
        }
        co_return sgrn::createTypedResponse(tsp_req,
            QuotaUpdateWithIdResponse{
                kind,
                row.id.value(),
                row.label,
                row.organisation,
                row.storage_used_bytes,
                row.storage_limit_bytes,
                row.entries_used,
                row.entry_count_limit,
            },
            k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

} // namespace sgrn::datastore::handlers::storage_admin
