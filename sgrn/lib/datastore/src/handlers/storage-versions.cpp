// HTTP endpoints for file version history.
// ============================================================================
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/utils/mime.hpp>
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;
namespace svc = sgrn::datastore::services::storage;

namespace
{
struct FileVersionItem {
    int64_t version_id{0};
    int64_t file_id{0};
    int32_t version_no{0};
    std::string storage_kind;
    bool is_checkpoint{false};
    std::string content_sha512;
    int64_t original_size{0};
    bool is_encrypted{false};
    std::optional<std::string> comment;
    std::optional<int64_t> restored_from_version_id;
    std::optional<int32_t> created_by_user_id;
    std::optional<int32_t> created_by_service_id;
    std::string created_at;
    bool is_current{false};
    int64_t stored_size{0};
};

struct RestoreRequest {
    std::string path;
    std::string scope{"personal"};
    int32_t version_no{0};
    std::optional<std::string> comment;
};
struct RestoreResponse {
    int64_t version_id{0};
    int32_t version_no{0};
    bool created{false};
};
struct TrimResponse {
    int32_t removed{0};
};

struct VersionTarget {
    int64_t file_id{0};
    std::string name;
    bool is_encrypted{false};
    svc::ScopeContext scope;
};

// Resolves scope + path to a file the caller may read (or write). Returns nullptr on success,
// otherwise the error response to send back.
Task<HttpResponsePtr> resolveVersionTarget(svc::StorageService& t_svc, const ::sgrn::datastore::auth::SessionClaims& t_session,
    const std::string& t_scope_str, const std::string& t_path, bool t_need_write, VersionTarget& t_out) {

    auto scope_res = co_await t_svc.resolveScopeSession(t_session, t_svc.parseScope(t_scope_str), t_path);

    CO_RETURN_IF(scope_res.hasError(), createTypedResponse(BackendErrorResponse{scope_res.error()}));

    CO_RETURN_IF(!scope_res->can_read || (t_need_write && !scope_res->can_write),
        createTypedResponse(BackendErrorResponse{"Privileges missing for this scope space.", k403Forbidden}));

    CO_RETURN_IF(scope_res->is_virtual_root, createTypedResponse(BackendErrorResponse{"Not a file", k400BadRequest}));

    auto db = t_svc.getDbClient();

    CO_RETURN_IF(db.hasError(), createTypedResponse(BackendErrorResponse{db.error()}));

    auto rec = co_await svc::helpers::fetchFileRecord(db.value(), *scope_res);

    CO_RETURN_IF(rec.hasError(), createTypedResponse(BackendErrorResponse{rec.error()}));

    t_out.file_id = rec->id;
    t_out.name = std::move(rec->name);
    t_out.is_encrypted = rec->is_encrypted;
    t_out.scope = std::move(scope_res.value());
    co_return nullptr;
}

std::string pathParam(const HttpRequestPtr& t_req) {
    return t_req->getOptionalParameter<std::string>("path").value_or("");
}

std::optional<std::string> normalizeVersionPath(std::string t_path) {
    if (t_path.empty()) {
        return "/";
    }
    if (t_path.find("..") != std::string::npos) {
        return std::nullopt;
    }
    if (t_path.front() != '/') {
        t_path.insert(t_path.begin(), '/');
    }
    if (t_path.size() > 1 && t_path.back() == '/') {
        t_path.pop_back();
    }
    return t_path;
}
} // namespace

// GET /api/v1/storage/files/versions?path=&scope=
Task<HttpResponsePtr> StorageApiHandler::handleListVersions(HttpRequestPtr tsp_req) {
    if (!tsp_req->attributes()->find("session_claims")) {
        co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
    }
    auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
    auto norm = normalizeVersionPath(pathParam(tsp_req));

    CO_RETURN_IF(!norm || *norm == "/", createTypedResponse(BackendErrorResponse{"Invalid path", k400BadRequest}));

    VersionTarget target;

    if (auto err = co_await resolveVersionTarget(
            storage_service_, session, tsp_req->getOptionalParameter<std::string>("scope").value_or("personal"), *norm, false, target)) {
        co_return err;
    }
    auto db = storage_service_.getDbClient();

    CO_RETURN_IF(db.hasError(), createTypedResponse(BackendErrorResponse{db.error()}));

    auto existing = co_await db.value()->execSqlCoro("SELECT 1 FROM storage.file_versions WHERE file_id = $1 LIMIT 1", target.file_id);

    if (existing.empty()) {
        co_await db.value()->execSqlCoro("SELECT storage.snapshot_head_as_version($1, $2, $3)", target.file_id, target.scope.user_id,
            target.scope.t_automated_service_id);
    }
    auto rows = co_await db.value()->execSqlCoro(
        "SELECT version_id, file_id, version_no, storage_kind, is_checkpoint, content_sha512, original_size, is_encrypted, comment, "
        "restored_from_version_id, created_by_user_id, created_by_service_id, created_at, is_current, stored_size "
        "FROM storage.file_version_details WHERE file_id = $1 ORDER BY version_no DESC",
        target.file_id);
    // A file that predates versioning has no rows yet: it is implicitly a single current version.
    co_return createTypedResponse(tsp_req, sgrn::orm::fromResultByName<FileVersionItem>(rows), k200OK);
}

// GET /api/v1/storage/files?path=&scope=&version=N      (called from handleFileRequest's GET branch)
Task<HttpResponsePtr> StorageApiHandler::handleDownloadVersion(
    ::sgrn::datastore::auth::SessionClaims t_session, std::string t_scope, std::string t_path, int32_t t_version_no) {
    VersionTarget target;

    if (auto err = co_await resolveVersionTarget(storage_service_, t_session, t_scope, t_path, false, target)) {
        co_return err;
    }

    auto db = storage_service_.getDbClient();
    auto s3 = storage_service_.S3Client();

    CO_RETURN_IF(db.hasError() || s3.hasError(), createTypedResponse(BackendErrorResponse{"Storage unavailable", k503ServiceUnavailable}));

    auto v = co_await db.value()->execSqlCoro(
        "SELECT id, content_sha512, original_size FROM storage.file_versions WHERE file_id = $1 AND version_no = $2", target.file_id,
        t_version_no);
    if (v.empty()) {
        co_await db.value()->execSqlCoro("SELECT storage.snapshot_head_as_version($1, $2, $3)", target.file_id, target.scope.user_id,
            target.scope.t_automated_service_id);
        v = co_await db.value()->execSqlCoro(
            "SELECT id, content_sha512, original_size FROM storage.file_versions WHERE file_id = $1 AND version_no = $2", target.file_id,
            t_version_no);
    }
    CO_RETURN_IF(v.empty(), createTypedResponse(BackendErrorResponse{"Version not found", k404NotFound}));

    auto content =
        co_await svc::loadVersionContent(db.value(), s3.value(), svc::currentStorageConfig().default_bucket, v[0]["id"].as<int64_t>());

    CO_RETURN_IF(content.hasError(), createTypedResponse(BackendErrorResponse{content.error()}));

    std::string name = target.name;
    name.erase(
        std::remove_if(name.begin(), name.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0' || c == '"'; }), name.end());
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    // Logical bytes: already decrypted and decompressed -> no X-Compressed header.
    resp->setContentTypeString(sgrn::utils::mime::inferMimeType(target.name));
    resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", name));
    resp->addHeader("X-Version-No", std::to_string(t_version_no));
    resp->addHeader("X-Original-Size", std::to_string(content.value().size()));
    resp->addHeader("ETag", "\"" + v[0]["content_sha512"].as<std::string>() + "\"");
    resp->setBody(std::move(content.value()));
    co_return resp;
}

// POST /api/v1/storage/files/versions/restore   {path, scope?, version_no, comment?}
Task<HttpResponsePtr> StorageApiHandler::handleRestoreVersion(HttpRequestPtr tsp_req) {
    if (!tsp_req->attributes()->find("session_claims")) {
        co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
    }
    auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
    auto body = parseTypedRequest<RestoreRequest>(tsp_req);

    CO_RETURN_IF(body.hasError() || body->version_no <= 0,
        createTypedResponse(BackendErrorResponse{"path and version_no are required", k400BadRequest}));

    auto norm = normalizeVersionPath(body->path);

    CO_RETURN_IF(!norm, createTypedResponse(BackendErrorResponse{"Invalid path", k400BadRequest}));

    VersionTarget target;
    if (auto err = co_await resolveVersionTarget(storage_service_, session, body->scope, *norm, true, target)) {
        co_return err;
    }
    { // explicit HTTP codes for the two expected failures
        auto db = storage_service_.getDbClient();
        auto v = co_await db.value()->execSqlCoro("SELECT (fv.id = f.current_version_id) AS is_current FROM storage.file_versions fv "
                                                  "JOIN storage.files f ON f.id = fv.file_id WHERE fv.file_id = $1 AND fv.version_no = $2",
            target.file_id, body->version_no);
        CO_RETURN_IF(v.empty(), createTypedResponse(BackendErrorResponse{"Version not found", k404NotFound}));

        CO_RETURN_IF(v[0]["is_current"].as<bool>(), createTypedResponse(BackendErrorResponse{"Version is already current", k409Conflict}));
    }
    auto res = co_await svc::restoreVersion(storage_service_, target.file_id, body->version_no, target.is_encrypted, target.scope.user_id,
        target.scope.t_automated_service_id, body->comment);

    CO_RETURN_IF(res.hasError(), createTypedResponse(BackendErrorResponse{res.error()}));

    co_return createTypedResponse(RestoreResponse{res->version_id, res->version_no, res->created}, k200OK);
}

// DELETE /api/v1/storage/files/versions?path=&scope=&keep=N    (prune everything older than the newest N)
Task<HttpResponsePtr> StorageApiHandler::handleTrimVersions(HttpRequestPtr tsp_req) {

    CO_RETURN_IF(
        !tsp_req->attributes()->find("session_claims"), createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized}));

    auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
    const std::string keep_param = tsp_req->getOptionalParameter<std::string>("keep").value_or("");
    int keep = 0;
    const auto [end, error] = std::from_chars(keep_param.data(), keep_param.data() + keep_param.size(), keep);
    if (error != std::errc{} || end != keep_param.data() + keep_param.size()) {
        keep = 0;
    }
    auto norm = normalizeVersionPath(pathParam(tsp_req));

    CO_RETURN_IF(!norm || keep < 1, createTypedResponse(BackendErrorResponse{"path and keep (>= 1) are required", k400BadRequest}));

    VersionTarget target;

    if (auto err = co_await resolveVersionTarget(
            storage_service_, session, tsp_req->getOptionalParameter<std::string>("scope").value_or("personal"), *norm, true, target)) {
        co_return err;
    }

    CO_RETURN_IF(!target.scope.can_delete,
        createTypedResponse(BackendErrorResponse{"Delete privileges missing for this scope space.", k403Forbidden}));

    auto res = co_await svc::trimVersions(storage_service_, target.file_id, keep, std::nullopt);

    CO_RETURN_IF(res.hasError(), createTypedResponse(BackendErrorResponse{res.error()}));

    co_return createTypedResponse(TrimResponse{res.value()}, k200OK);
}

} // namespace sgrn::datastore::handlers::storage
