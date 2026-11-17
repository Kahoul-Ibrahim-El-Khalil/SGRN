// admin-users-services.cpp — User accounts and automated-service administration.
#include "admin-common.hpp"

#include <sgrn/datastore/core/orm-glaze.hpp>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
struct RegisterUserRequest {
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string password;
    std::string phone_number;
    std::string organisation;
    std::string status{"active"};
    std::string domain;
    std::optional<int64_t> storage_limit;
    std::string role{"user"};
    bool is_platform_admin{false};
};

struct RegisterAutomatedServiceRequest {
    std::string name;
    std::string kind;
    sgrn::json::RawJson metadata{"{}"};
    std::optional<int64_t> storage_limit;
    std::optional<std::string> domain;
};

struct UpdateAutomatedServiceMetadataRequest {
    std::optional<sgrn::json::RawJson> metadata;
    std::optional<std::string> description;
};

struct RotateAutomatedServiceTokenRequest {
    int32_t automated_service_id{0};
};

struct SetStatusRequest {
    std::string status;
};

struct ResetPasswordRequest {
    std::string new_password;
};

struct UserListEntry {
    int32_t id{0};
    std::string email;
    std::string first_name;
    std::string family_name;
    std::string organisation;
};

struct ServiceRegistrationResponse {
    std::string message;
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    std::string organisation;
    std::string kind;
};

struct ServiceRegistrationRow {
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    std::string organisation;
};

struct ServiceRotationRow {
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    std::string status;
    std::string organisation;
    std::string created_at;
    std::string metadata;
};

struct ServiceRotationOwnerRow {
    std::string organisation;
    std::optional<std::string> token;
};

struct ServiceTokenRow {
    std::optional<std::string> token;
};

struct ServiceRotationResponse {
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    bool is_active{false};
    std::string status;
    std::string organisation;
    std::string created_at;
    sgrn::json::RawJson metadata{"{}"};
    std::optional<sgrn::json::RawJson> kind;
};

struct UserStatusResponse {
    std::string status;
};
struct DeletedUserResponse {
    int32_t deleted_user_id{0};
};
struct SuccessResponse {};
struct ServiceStatusResponse {
    int32_t automated_service_id{0};
    std::string status;
};
struct DeletedServiceResponse {
    int32_t deleted_service_id{0};
};

Json::Value parseMetadataValue(const std::string& t_raw) {
    if (t_raw.empty()) {
        return Json::Value(Json::objectValue);
    }
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value metadata(Json::objectValue);
    std::string errors;
    if (!reader->parse(t_raw.c_str(), t_raw.c_str() + t_raw.size(), &metadata, &errors)) {
        return Json::Value(Json::objectValue);
    }
    return metadata;
}

struct UserTarget {
    int32_t id{};
    std::string organisation;
    std::string email;
    bool is_platform_admin{false};
    std::string status;
};

struct OrgAdminStatus {
    bool is_admin{false};
    int64_t other_admins{0};
};

static constexpr char kOrgAdminStatusQuery[] =
    "SELECT EXISTS (SELECT 1 FROM core.admin_assignments WHERE user_id = $1 AND scope = 'organisation') AS is_admin, "
    "(SELECT count(*) FROM core.admin_assignments a JOIN core.users u ON u.id = a.user_id "
    " WHERE a.scope = 'organisation' AND a.organisation = (SELECT organisation FROM core.users WHERE id = $1) "
    "   AND u.status = 'active' AND u.deleted_at IS NULL AND u.id <> $1) AS other_admins";

// Resolves an active (non-soft-deleted) user. Returns nullopt for "missing"
// AND "belongs to another organisation" so callers answer identically (no
// tenant enumeration).
drogon::Task<std::optional<UserTarget>> loadUserTarget(
    const drogon::orm::DbClientPtr& t_db, int32_t t_id, const std::string& t_caller_org, bool t_is_platform) {
    static constexpr char kQuery[] =
        "SELECT id, organisation, email, is_platform_admin, status FROM core.users WHERE id = $1 AND deleted_at IS NULL";
    auto rows = co_await t_db->execSqlCoro(kQuery, t_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    UserTarget target = sgrn::orm::fromRow<UserTarget, kQuery>(rows[0]);
    if (!t_is_platform && target.organisation != t_caller_org) {
        co_return std::nullopt;
    }
    co_return target;
}

// True when the user holds an organisation-scope admin assignment and nobody
// else in that organisation can still administer it.
drogon::Task<bool> isLastOrgAdmin(const drogon::orm::DbClientPtr& t_db, int32_t t_user_id) {
    auto rows = co_await t_db->execSqlCoro(kOrgAdminStatusQuery, t_user_id);
    if (rows.empty()) {
        co_return false;
    }
    const auto status = sgrn::orm::fromRow<OrgAdminStatus, kOrgAdminStatusQuery>(rows[0]);
    co_return status.is_admin&& status.other_admins == 0;
}

} // namespace detail
using namespace detail;

Task<HttpResponsePtr> AdminApiHandler::handleGetUsers(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);
        const std::string org_param = tsp_req->getParameter("organisation");

        std::string target_org;
        if (!is_platform) {
            if (!org_param.empty() && org_param != session_org) {
                co_return createTypedResponse(
                    BackendErrorResponse{"Forbidden: cannot list users from another organisation", k403Forbidden, "AdminApi"});
            }
            target_org = session_org;
        } else {
            target_org = org_param;
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        static constexpr char kAllUsers[] =
            "SELECT id, email, first_name, family_name, organisation FROM core.users WHERE deleted_at IS NULL ORDER BY id";
        static constexpr char kOrgUsers[] = "SELECT id, email, first_name, family_name, organisation FROM core.users "
                                            "WHERE deleted_at IS NULL AND organisation = $1 ORDER BY id";

        std::vector<UserListEntry> users;
        if (!target_org.empty()) {
            users = sgrn::orm::fromResult<UserListEntry, kOrgUsers>(co_await db->execSqlCoro(kOrgUsers, target_org));
        } else {
            users = sgrn::orm::fromResult<UserListEntry, kAllUsers>(co_await db->execSqlCoro(kAllUsers));
        }

        co_return createTypedResponse(tsp_req, users, k200OK);
    } catch (const std::exception& e) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterUser(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<RegisterUserRequest>(tsp_req);
    CO_RETURN_IF(req_res.hasError(), createTypedResponse(BackendErrorResponse{"Invalid JSON payload", k400BadRequest}));
    const auto& req = req_res.value();

    RegisterUserPayload payload;
    payload.first_name = sgrn::utils::strings::toPascalCase(sgrn::utils::strings::trim(req.first_name));
    payload.family_name = sgrn::utils::strings::toPascalCase(sgrn::utils::strings::trim(req.family_name));
    payload.email = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(req.email));
    payload.password = req.password;
    payload.phone_number = sgrn::utils::strings::trim(req.phone_number);
    payload.organisation = sgrn::utils::strings::trim(req.organisation);
    payload.status = sgrn::utils::strings::trim(req.status.empty() ? "active" : req.status);
    payload.domain = sgrn::utils::strings::trim(req.domain);
    payload.storage_limit = req.storage_limit;

    std::string error_msg;
    if (!validateRegisterUserPayload(payload, error_msg)) {
        co_return createTypedResponse(BackendErrorResponse{error_msg, k400BadRequest});
    }

    const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
    const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

    if (!is_platform) {
        if (req.is_platform_admin) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot grant platform admin status (escalation prohibited)", k403Forbidden, "AdminApi"});
        }
        std::string req_role = req.role.empty() ? "user" : req.role;
        if (req_role == "platform_admin") {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot grant platform admin role (escalation prohibited)", k403Forbidden, "AdminApi"});
        }
        if (!payload.organisation.empty() && payload.organisation != session_org) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot register user in another organisation", k403Forbidden, "AdminApi"});
        }
        payload.organisation = session_org;
    } else {
        if (payload.organisation.empty()) {
            payload.organisation = session_org;
        }
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();
        payload.domain = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(payload.domain));
        if (!payload.domain.empty()) {
            auto dom = co_await db->execSqlCoro(
                "SELECT 1 FROM core.domains WHERE organisation = $1 AND name = $2", payload.organisation, payload.domain);
            if (dom.empty()) {
                co_return createTypedResponse(BackendErrorResponse{
                    "Unknown domain '" + payload.domain + "' in organisation '" + payload.organisation + "'", k400BadRequest, "AdminApi"});
            }
        }
        std::string role = req.role.empty() ? "user" : req.role;
        auto result = co_await registerUser(db, payload, role);
        CO_RETURN_IF(result.hasError(), createTypedResponse(BackendErrorResponse{result.error(), k400BadRequest}));

        const auto user = std::move(result).value();
        const auto ctx = detail::requestContext(tsp_req);
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.created",
            "user", std::to_string(user.id), tsp_req->getPeerAddr().toIp(), req, "success");

        co_return createTypedResponse(
            tsp_req, ::sgrn::datastore::admin::RegisterUserResponse{true, "User registered successfully", user}, k201Created);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("User registration error: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("User registration failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterAutomatedService(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<RegisterAutomatedServiceRequest>(tsp_req);
    CO_RETURN_IF(req_res.hasError(), createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload}));
    const auto& req = req_res.value();

    std::string name = sgrn::utils::strings::trim(req.name);
    std::string kind = sgrn::utils::strings::trim(req.kind);

    if (name.empty()) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }

    try {
        // Bind automated services to the creator's organisation by default.
        const auto ctx = detail::requestContext(tsp_req);
        const std::string org = ctx.organisation;
        if (org.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidSession});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        // Create automated service via DB function (generates token + token_secret securely).
        // `kind` is stored in metadata for compatibility with the older admin UI.
        std::string meta_str = req.metadata.str.empty() ? "{}" : req.metadata.str;
        if (!kind.empty()) {
            std::unordered_map<std::string, glz::raw_json> meta_obj;
            if (!glz::read_json(meta_obj, meta_str)) {
                std::string kind_json;
                (void)glz::write_json(kind, kind_json);
                meta_obj["kind"] = glz::raw_json{kind_json};
                std::string serialized;
                (void)glz::write_json(meta_obj, serialized);
                meta_str = std::move(serialized);
            }
        }

        std::optional<int64_t> storage_limit = req.storage_limit;
        std::optional<std::string> domain = req.domain;
        if (!domain.has_value()) {
            if (tsp_req->attributes()->find("auth_context")) {
                const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
                if (auth.user.domain.has_value() && !auth.user.domain->empty()) {
                    domain = auth.user.domain;
                }
            } else if (tsp_req->attributes()->find("session_claims")) {
                domain = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user.domain;
            }
        }

        static constexpr char kCreateAutomatedServiceQuery[] = "SELECT id AS automated_service_id, name, token, token_secret, organisation "
                                                               "FROM core.create_automated_service($1, $2::jsonb, $3, $4, $5)";
        auto res = co_await db->execSqlCoro(kCreateAutomatedServiceQuery, name, meta_str, org, storage_limit, domain);

        if (res.empty())
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});

        const auto service = sgrn::orm::fromRow<ServiceRegistrationRow, kCreateAutomatedServiceQuery>(res[0]);
        ServiceRegistrationResponse resp{"Automated service registered successfully", service.automated_service_id, service.name,
            service.token, service.token_secret, service.organisation, kind};

        // Sync: insert the new public service token into local RAM sorted vector
        // and broadcast PG NOTIFY so all cluster nodes update their lookup vectors.
        const std::string new_serv_token = service.token;
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_serv_token);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_serv_token);

        const int32_t actor_id = ctx.actor_id;
        const std::string actor_name = ctx.actor_name;
        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "service.created", "automated_service",
            std::to_string(service.automated_service_id), tsp_req->getPeerAddr().toIp(), req, "success");

        co_return createTypedResponse(tsp_req, resp, k201Created);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Automated service registration DB error: {}", e.base().what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Automated service registration failed: ") + e.base().what(), k400BadRequest, "AdminApi"});
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Automated service registration error: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Automated service registration failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateAutomatedServiceMetadata(HttpRequestPtr tsp_req, std::string t_id) {
    const std::string id_str = std::move(t_id);
    auto req_res = parseTypedRequest<UpdateAutomatedServiceMetadataRequest>(tsp_req);
    if (id_str.empty() || req_res.hasError() || (!req_res->metadata.has_value() && !req_res->description.has_value())) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }
    const auto& req = req_res.value();

    int32_t automated_service_id;
    try {
        automated_service_id = std::stoi(id_str);
    } catch (const std::exception&) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }
    const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);

    auto db_res = sgrn::datastore::core::getDbClient();
    CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
    auto db = db_res.value();

    try {
        // Description update rides the same PATCH: the service registry
        // previously had no way to change the separate description column
        // after registration. Empty string clears it.
        const bool has_description = req.description.has_value();
        const std::string description = has_description ? sgrn::utils::strings::trim(*req.description) : "";

        const std::string meta_str = req.metadata.has_value() ? req.metadata->str : "{}";

        // Update with ORG check to ensure an admin from Org A cannot edit automated services from Org B.
        size_t affected = 0;
        if (has_description) {
            std::optional<std::string> desc_param = description.empty() ? std::nullopt : std::optional<std::string>(description);
            auto r = co_await db->execSqlCoro(
                "UPDATE core.automated_services SET metadata = metadata || $1::jsonb, description = $3 WHERE id = $2 AND organisation = $4",
                meta_str, automated_service_id, desc_param, org);
            affected = r.affectedRows();
        } else {
            auto r = co_await db->execSqlCoro(
                "UPDATE core.automated_services SET metadata = metadata || $1::jsonb WHERE id = $2 AND organisation = $3", meta_str,
                automated_service_id, org);
            affected = r.affectedRows();
        }

        if (affected == 0) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        co_return createTypedResponse(tsp_req, MessageResponse{"Automated service metadata updated successfully"}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to update automated service metadata: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to update automated service metadata: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRotateAutomatedServiceToken(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<RotateAutomatedServiceTokenRequest>(tsp_req);
    if (req_res.hasError() || req_res->automated_service_id <= 0) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }

    const int32_t automated_service_id = req_res->automated_service_id;
    if (automated_service_id <= 0) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
    }

    const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);
    if (org.empty()) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidSession});
    }

    auto db_res = sgrn::datastore::core::getDbClient();
    CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
    auto db = db_res.value();

    try {
        static constexpr char kServiceRotationOwnerQuery[] =
            "SELECT organisation, token::text AS token FROM core.automated_services WHERE id = $1";
        auto owner_res = co_await db->execSqlCoro(kServiceRotationOwnerQuery, automated_service_id);
        if (owner_res.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        const auto owner = sgrn::orm::fromRow<ServiceRotationOwnerRow, kServiceRotationOwnerQuery>(owner_res[0]);
        if (owner.organisation != org) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        const std::string old_tok = owner.token.value_or("");

        static constexpr char kRotateServiceCredentialsQuery[] =
            "SELECT id AS automated_service_id, name, token, token_secret, status, organisation, created_at, metadata::text AS metadata "
            "FROM core.rotate_automated_service_credentials($1, true)";
        auto rotation = co_await db->execSqlCoro(kRotateServiceCredentialsQuery, automated_service_id);
        if (rotation.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
        }

        const auto updated = sgrn::orm::fromRow<ServiceRotationRow, kRotateServiceCredentialsQuery>(rotation[0]);
        const Json::Value metadata = parseMetadataValue(updated.metadata);
        Json::StreamWriterBuilder metadata_writer;
        metadata_writer["indentation"] = "";
        const sgrn::json::RawJson metadata_raw(Json::writeString(metadata_writer, metadata));
        std::optional<sgrn::json::RawJson> kind_raw;
        if (metadata.isObject() && metadata.isMember("kind") && !metadata["kind"].isNull()) {
            kind_raw = sgrn::json::RawJson(Json::writeString(metadata_writer, metadata["kind"]));
        }
        ServiceRotationResponse resp{updated.automated_service_id, updated.name, updated.token, updated.token_secret,
            updated.status == "active", updated.status, updated.organisation, updated.created_at, metadata_raw, std::move(kind_raw)};

        // Sync: old public token is replaced — deregister old UUID from sorted vector
        // and register new UUID, broadcasting PG NOTIFY to all cluster nodes.
        if (!old_tok.empty()) {
            sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(old_tok);
            co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + old_tok);
        }
        const std::string new_tok = updated.token;
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_tok);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_tok);

        co_return createTypedResponse(tsp_req, resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to rotate credentials: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to rotate credentials: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleSetUserStatus(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto req_res = parseTypedRequest<SetStatusRequest>(tsp_req);
        if (req_res.hasError() || req_res->status.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const std::string& status = req_res->status;
        if (status != "active" && status != "suspended") {
            co_return createTypedResponse(BackendErrorResponse{"status must be 'active' or 'suspended'", k400BadRequest, "AdminApi"});
        }
        if (target_id == ctx.actor_id) {
            co_return createTypedResponse(BackendErrorResponse{"Forbidden: you cannot change your own status", k403Forbidden, "AdminApi"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: only a platform admin may change a platform admin", k403Forbidden, "AdminApi"});
        }
        if (status == "suspended" && co_await isLastOrgAdmin(db, target_id)) {
            co_return createTypedResponse(
                BackendErrorResponse{"Conflict: cannot suspend the last organisation admin", k409Conflict, "AdminApi"});
        }

        // trg_revoke_user_sessions terminates live sessions when status leaves
        // 'active' or is_active drops (WP2 migration).
        co_await db->execSqlCoro(
            "UPDATE core.users SET status = $2::text, is_active = ($2::text = 'active') WHERE id = $1", target_id, status);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.status_changed",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), *req_res, "success");

        co_return createTypedResponse(tsp_req, UserStatusResponse{status}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to set user status: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to set user status: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteUser(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        if (target_id == ctx.actor_id) {
            co_return createTypedResponse(BackendErrorResponse{"Forbidden: you cannot delete your own account", k403Forbidden, "AdminApi"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: only a platform admin may delete a platform admin", k403Forbidden, "AdminApi"});
        }
        if (target->status == "active" && co_await isLastOrgAdmin(db, target_id)) {
            co_return createTypedResponse(
                BackendErrorResponse{"Conflict: cannot delete the last organisation admin", k409Conflict, "AdminApi"});
        }

        // Soft delete: trg_revoke_user_sessions ends live sessions.
        co_await db->execSqlCoro("UPDATE core.users SET deleted_at = now(), is_active = false WHERE id = $1", target_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.deleted",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");
        co_return createTypedResponse(tsp_req, DeletedUserResponse{target_id}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to delete user: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{std::string("Failed to delete user: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleResetUserPassword(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto req_res = parseTypedRequest<ResetPasswordRequest>(tsp_req);
        if (req_res.hasError() || req_res->new_password.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const std::string& new_password = req_res->new_password;

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: only a platform admin may reset a platform admin's password", k403Forbidden, "AdminApi"});
        }
        if (new_password.size() < 12) {
            co_return createTypedResponse(BackendErrorResponse{"new_password must be at least 12 characters", k400BadRequest, "AdminApi"});
        }
        if (sgrn::utils::strings::toLower(new_password) == sgrn::utils::strings::toLower(target->email)) {
            co_return createTypedResponse(
                BackendErrorResponse{"new_password must differ from the user's email", k400BadRequest, "AdminApi"});
        }

        // trg_hash_password only runs BEFORE INSERT — hash explicitly here.
        co_await db->execSqlCoro("UPDATE core.users SET password = crypt($2, gen_salt('bf')) WHERE id = $1", target_id, new_password);
        // Sessions do not survive a credential change; the enqueue trigger
        // broadcasts the revoke to every instance.
        co_await db->execSqlCoro("UPDATE core.sessions SET terminated_at = now(), termination_reason = 'password_changed' "
                                 "WHERE user_id = $1 AND terminated_at IS NULL",
            target_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        Json::Value details(Json::objectValue); // never the password itself
        details["sessions_revoked"] = true;
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.password_reset",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), details, "success");

        co_return createTypedResponse(tsp_req, SuccessResponse{}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to reset user password: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to reset user password: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleSetAutomatedServiceStatus(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t service_id = 0;
        try {
            service_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto req_res = parseTypedRequest<SetStatusRequest>(tsp_req);
        if (req_res.hasError() || req_res->status.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const std::string& status = req_res->status;
        if (status != "active" && status != "degraded" && status != "offline" && status != "maintenance") {
            co_return createTypedResponse(
                BackendErrorResponse{"status must be one of 'active', 'degraded', 'offline' or 'maintenance'", k400BadRequest, "AdminApi"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        // Org-scoped unless platform admin. Token registration (SessionStore)
        // keys on status = 'active' AND deleted_at IS NULL, so a non-active
        // service loses its credential lookup immediately — the same gate
        // loadActiveTokens applies at boot.
        std::string sql = "UPDATE core.automated_services SET status = $1::text "
                          "WHERE id = $2 AND deleted_at IS NULL RETURNING token::text";
        std::vector<std::string> binds = {status, std::to_string(service_id)};
        if (!is_platform) {
            sql += " AND organisation = $3";
            binds.push_back(ctx.organisation);
        }
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);
        if (res.empty()) {
            // missing OR other tenant — same answer
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        const std::string token = sgrn::orm::fromRowByName<ServiceTokenRow>(res[0]).token.value_or("");
        if (!token.empty()) {
            if (status == "active") {
                sgrn::datastore::session::SessionStore::instance().registerServiceToken(token);
                co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + token);
            } else {
                sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(token);
                co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + token);
            }
        }

        Json::Value details(Json::objectValue);
        details["status"] = status;
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name,
            "service.status_changed", "automated_service", std::to_string(service_id), tsp_req->getPeerAddr().toIp(), details, "success");

        co_return createTypedResponse(tsp_req, ServiceStatusResponse{service_id, status}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to set automated service status: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to set automated service status: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteAutomatedService(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t service_id = 0;
        try {
            service_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        std::string sql =
            "UPDATE core.automated_services SET deleted_at = now() WHERE id = $1 AND deleted_at IS NULL RETURNING token::text";
        std::vector<std::string> binds = {std::to_string(service_id)};
        if (!is_platform) {
            binds.push_back(ctx.organisation);
            sql += " AND organisation = $2";
        }
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);
        if (res.empty()) {
            // missing OR other tenant — same answer
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        const std::string token = sgrn::orm::fromRowByName<ServiceTokenRow>(res[0]).token.value_or("");
        if (!token.empty()) {
            sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(token);
            co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + token);
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "service.deleted",
            "automated_service", std::to_string(service_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");
        co_return createTypedResponse(tsp_req, DeletedServiceResponse{service_id}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to delete automated service: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to delete automated service: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

} // namespace sgrn::datastore::handlers::admin
