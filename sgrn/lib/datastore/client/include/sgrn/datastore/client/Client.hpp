#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/client/StorageClient.hpp>
#include <sgrn/datastore/client/Types.hpp>
#include <sgrn/datastore/wire/admin.hpp>
#include <sgrn/datastore/wire/codec.hpp>
#include <sgrn/utils/glaze.hpp>
#include <sgrn/utils/threading.hpp>
#include <httplib.h>
#include <stdexcept>

namespace sgrn::datastore::client
{

enum class AuthMode {
    AutomatedService,
    UserPassword,
    SessionToken,
};

// ─── Configuration ───────────────────────────────────────────────────────────

struct AutomatedServiceConfig {
    std::string backend_url;
    AuthMode auth_mode{AuthMode::AutomatedService};

    std::string public_token;
    std::string private_token;
    std::string token;
    std::string secret;

    std::string email;
    std::string password;
    std::string session_token;

    std::string user_storage_path{"/api/v1/storage/files"};
    std::string service_storage_path{"/api/v1/storage/automated-service/files"};
    std::string storage_path{"/api/v1/storage/automated-service/files"};
    double storage_timeout_s{30.0};

    bool retry_on_unauthorized{true};
    uint8_t max_unauthorized_retries{1};
};

namespace detail
{

inline std::string authModeToString(AuthMode t_mode) {
    switch (t_mode) {
        case AuthMode::UserPassword:
            return "user-password";
        case AuthMode::SessionToken:
            return "session-token";
        case AuthMode::AutomatedService:
        default:
            return "automated-service";
    }
}

inline std::string resolveDriveBase(const AutomatedServiceConfig& t_config) {
    if (t_config.auth_mode == AuthMode::UserPassword)
        return "/api/v1/storage/drive";
    return "/api/v1/storage/automated-service/drive";
}

inline std::string resolveStoragePath(const AutomatedServiceConfig& t_config) {
    if (!t_config.storage_path.empty() && t_config.storage_path != "/api/v1/storage/automated-service/files") {
        return t_config.storage_path;
    }
    switch (t_config.auth_mode) {
        case AuthMode::UserPassword:
            return t_config.user_storage_path.empty() ? "/api/v1/storage/files" : t_config.user_storage_path;
        case AuthMode::SessionToken:
        case AuthMode::AutomatedService:
        default:
            return t_config.service_storage_path.empty() ? "/api/v1/storage/automated-service/files" : t_config.service_storage_path;
    }
}

inline std::string_view effectivePublicToken(const AutomatedServiceConfig& t_config) {
    return !t_config.public_token.empty() ? std::string_view{t_config.public_token} : std::string_view{t_config.token};
}

inline std::string_view effectivePrivateToken(const AutomatedServiceConfig& t_config) {
    return !t_config.private_token.empty() ? std::string_view{t_config.private_token} : std::string_view{t_config.secret};
}

} // namespace detail

using DatastoreClientConfig = AutomatedServiceConfig;
using SgrnClientConfig = AutomatedServiceConfig;

// Forward declarations
class StorageClient;

/**
 * @brief DatastoreClient
 * The unified asynchronous hub for accessing SGRN datastore infrastructure.
 */
class DatastoreClient {
public:
    explicit DatastoreClient(DatastoreClientConfig t_config);
    ~DatastoreClient();

    /**
     * @brief Access domain-specific clients (lightweight views).
     */
    StorageClient& storage();

    /**
     * @brief Unified asynchronous task publishing.
     */
    void uploadFileAsync(const std::string& t_remote_path, const std::string& t_local_path);

    /**
     * @brief Generic data table query (Synchronous).
     * @return The parsed response, or a null Value on failure.
     */
    sgrn::json::Value query(const std::string& t_table, const std::string& t_params = "");

    /**
     * @brief Account-level listings backing domain navigation (Synchronous).
     * Tenant-scoped server-side from the session; no organisation parameter
     * is sent (or trusted) anymore.
     */
    std::vector<IdNamePair> listOrganisations();
    std::vector<IdNamePair> listDomains();
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListOrganisations();
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListDomains();
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListStatuses(const std::string& t_organisation);

    /// Account admin.
    sgrn::Result<std::vector<AdminUserEntry>, std::string> tryListUsers();
    sgrn::Result<std::string, std::string> registerUser(const NewUser& t_user);
    sgrn::Result<std::vector<ServiceEntry>, std::string> tryListServices();
    sgrn::Result<ServiceCredentials, std::string> registerService(const NewService& t_service);
    sgrn::Result<ServiceCredentials, std::string> rotateServiceToken(int64_t t_service_id);

    /// Session: sign out and password change.
    bool signOut();
    sgrn::Result<std::string, std::string> updatePassword(const std::string& t_old_password, const std::string& t_new_password);

    /// Raw JSON passthroughs for profile display.
    sgrn::Result<std::string, std::string> userInfoJson();
    sgrn::Result<std::string, std::string> storageStatsJson();
    sgrn::Result<std::string, std::string> storageConstraintsJson();

    /// Garage storage administration (admin-only endpoints). Raw JSON
    /// passthroughs; the shell renders them as tables / pretty JSON.
    sgrn::Result<std::string, std::string> storageAdminOverviewJson(uint32_t t_max_pages = 10);
    sgrn::Result<std::string, std::string> storageAdminOrphansJson(
        const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit = 500, uint32_t t_max_pages = 10);
    sgrn::Result<std::string, std::string> storageAdminPurgeOrphansJson(const std::string& t_bucket, const std::vector<std::string>& t_keys,
        const std::string& t_prefix, bool t_dry_run, uint32_t t_limit = 500);
    sgrn::Result<std::string, std::string> storageAdminSearchJson(
        const std::string& t_bucket, const std::string& t_key, const std::string& t_prefix, uint32_t t_limit = 100);

    /// Resumable upload session management.
    struct ResumableUploadSession {
        std::string upload_id;
        int64_t chunk_size{5 * 1024 * 1024};
        int32_t total_chunks{0};
        std::string status;
        bool is_encrypted{false};
    };
    struct ResumableUploadStatus {
        std::string upload_id;
        std::string filename;
        std::string target_path;
        int64_t total_size{0};
        int64_t chunk_size{0};
        int32_t total_chunks{0};
        int32_t uploaded_chunks_count{0};
        std::string status;
        std::vector<int32_t> uploaded_chunk_indices;
    };
    sgrn::Result<ResumableUploadSession, std::string> initUploadSession(const std::string& t_filename, const std::string& t_target_path,
        int64_t t_total_size, int64_t t_chunk_size = 5 * 1024 * 1024, const std::string& t_mime_type = "application/octet-stream",
        bool t_encrypt = false);
    sgrn::Result<bool, std::string> uploadChunk(const std::string& t_upload_id, int32_t t_chunk_index, const char* t_data, size_t t_size);
    sgrn::Result<ResumableUploadStatus, std::string> getUploadStatus(const std::string& t_upload_id);
    sgrn::Result<std::string, std::string> completeUploadSession(const std::string& t_upload_id);
    sgrn::Result<std::string, std::string> abortUploadSession(const std::string& t_upload_id);

    /// Live session listing (admin: metaprobe).
    sgrn::Result<std::string, std::string> listActiveSessionsJson();
    /// Typed variant of listActiveSessionsJson() (shared wire type).
    sgrn::Result<std::vector<sgrn::datastore::admin::ActiveSession>, std::string> tryListActiveSessions();

    /// Webhook management (admin).
    sgrn::Result<std::string, std::string> listWebhooksJson();
    /// Typed variant of listWebhooksJson() (shared wire type).
    sgrn::Result<std::vector<sgrn::datastore::admin::Webhook>, std::string> tryListWebhooks();
    sgrn::Result<std::string, std::string> registerWebhookJson(const std::string& t_url, const std::string& t_secret = "");
    sgrn::Result<std::string, std::string> deleteWebhookJson(int32_t t_webhook_id);

    /// Typed parses of the admin storage reports above (for table output).
    sgrn::Result<std::vector<StorageBucketCensus>, std::string> tryStorageAdminOverview(uint32_t t_max_pages = 10);
    sgrn::Result<StorageOrphansReport, std::string> tryStorageAdminOrphans(
        const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit = 500, uint32_t t_max_pages = 10);
    sgrn::Result<StoragePurgeResult, std::string> tryStorageAdminPurge(const std::string& t_bucket, const std::vector<std::string>& t_keys,
        const std::string& t_prefix, bool t_dry_run, uint32_t t_limit = 500);

    const DatastoreClientConfig& config() const {
        return config_;
    }

    // ── Internal implementation helpers (used by Domain views) ─────────────────
    bool signIn();
    bool hasSessionToken() const;
    std::string getSessionToken() const;
    void setSessionToken(std::string t_token);
    void clearSessionToken();

    /// Perform a request and return the parsed JSON body, or a null Value on
    /// any failure (transport error, non-JSON body). Null is the historical
    /// "empty response" sentinel callers test for.
    sgrn::json::Value makeRequest(const std::string& t_method, const std::string& t_url, const std::string& t_body = "");
    sgrn::json::Value makeRequest(
        const std::string& t_method, const std::string& t_url, const std::string& t_body, const std::string& t_content_type);

    /// Typed binary request: sends Accept: application/x-beve and deserializes
    /// the response directly into T using Glaze reflection (fastest path, no
    /// JSON intermediary). A non-BEVE 2xx response (older server that ignored
    /// Accept) is decoded as JSON. An HTTP >= 400 response is parsed as JSON
    /// and reported as "HTTP <status>: <error>" (or "HTTP <status>" without a
    /// server error message); a failed BEVE decode is returned verbatim
    /// instead of being retried as JSON, which would mask the real error.
    template <typename T>
    sgrn::Result<T, std::string> makeTypedRequest(const std::string& t_method, const std::string& t_url, const std::string& t_body = "",
        const std::string& t_content_type = "application/json");

    struct DownloadResult {
        bool ok{false};
        int http_status{0};
        std::string bytes;
        std::string content_type;
        std::string message;
    };
    DownloadResult doDownload(const std::string& t_remote_path, StorageScope t_scope = StorageScope::Auto);
    DownloadResult doDownloadDriveZip(const std::string& t_path, StorageScope t_scope);

    struct UploadResult {
        bool ok{false};
        int http_status{0};
        std::string message;
        std::optional<int64_t> file_id;
        std::optional<std::string> key;
    };
    UploadResult doUpload(const std::string& t_remote_path, std::string t_bytes, StorageScope t_scope = StorageScope::Auto);

    DriveListing listDrive(const std::string& t_path = "/", StorageScope t_scope = StorageScope::Auto);
    sgrn::Result<DriveListing, std::string> tryListDrive(const std::string& t_path = "/", StorageScope t_scope = StorageScope::Auto);
    bool createDriveDirectory(const std::string& t_path, StorageScope t_scope = StorageScope::Auto);
    bool moveDriveItem(int64_t t_id, DriveItemType t_type, const std::string& t_new_name = "",
        std::optional<int64_t> t_target_parent_id = std::nullopt, StorageScope t_scope = StorageScope::Auto);
    bool deleteDriveItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope = StorageScope::Auto);

private:
    bool signInAutomatedService();
    bool signInUser();
    bool signInSessionToken();

    enum class TaskType { UploadFile };
    struct Task {
        TaskType type;
        std::string identifier;
        std::string data;
        uint8_t retries{0};
    };

    void processTask(const Task& t_task);

    std::unique_ptr<StorageClient> storage_client_;

    DatastoreClientConfig config_;

    mutable std::mutex session_token_mu_;
    std::string session_token_;

    std::unique_ptr<sgrn::utils::DynamicThreadPool> pool_;

    std::unique_ptr<httplib::Client> http_client_;
};

// Aliases for compatibility
using SgrnClient = DatastoreClient;
using SpaceClient = DatastoreClient;
using AutomatedService = DatastoreClient;

// ---------------------------------------------------------------------------
// makeTypedRequest — inline template definition
// Defined here (not in Client.cpp) so the linker sees the instantiation for
// every T without explicit instantiation declarations.
// ---------------------------------------------------------------------------
template <typename T>
sgrn::Result<T, std::string> DatastoreClient::makeTypedRequest(
    const std::string& t_method, const std::string& t_url, const std::string& t_body, const std::string& t_content_type) {
    if (!hasSessionToken())
        signIn();

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());
    // No Accept header: the server negotiates its default binary format and
    // the response Content-Type selects the decoder. Callers that need a
    // specific format set their own Accept header.

    auto doRequest = [&]() -> httplib::Result {
        if (t_method == "GET")
            return http_client_->Get(t_url, headers);
        if (t_method == "POST")
            return http_client_->Post(t_url, headers, t_body, t_content_type);
        if (t_method == "PATCH")
            return http_client_->Patch(t_url, headers, t_body, t_content_type);
        if (t_method == "DELETE")
            return http_client_->Delete(t_url, headers);
        return {};
    };

    httplib::Result res = doRequest();

    if (res && (res->status == 401 || res->status == 403) && config_.retry_on_unauthorized) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res = doRequest();
        } else {
            return sgrn::Result<T, std::string>::Error("Re-auth failed");
        }
    }

    RETURN_IF(!res, Error("No response"));

    // Error bodies are always JSON (the server never sends binary errors).
    // Surface the server's "error" string so callers see why, not just a code.
    if (res->status >= 400) {
        std::string error_text;
        auto parsed = sgrn::json::parseValue(res->body);
        if (!parsed.hasError()) {
            if (const sgrn::json::Value* err = sgrn::json::find(parsed.value(), "error"); err != nullptr && err->is_string()) {
                error_text = err->get_string();
            }
        }

        RETURN_IF(!error_text.empty(), Error(fmt::format("HTTP {}: {}", res->status, error_text)));

        return Error(fmt::format("HTTP {}", res->status));
    }

    // The server decides the format; Content-Type decides the decoder. This is
    // the only wire-format branch a client needs (see wire/codec.hpp).
    const std::string& ct = res->get_header_value("Content-Type");
    auto decoded = sgrn::wire::decode<T>(res->body, ct);
    RETURN_IF(decoded.hasError(), Error(fmt::format("Response decode failed ({}; {} bytes): {}", ct.empty() ? "no Content-Type" : ct,
                                      res->body.size(), decoded.error().what())));

    return std::move(decoded).value();
}

} // namespace sgrn::datastore::client
