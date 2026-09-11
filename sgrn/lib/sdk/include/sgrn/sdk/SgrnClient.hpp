#pragma once

#include <fmt/format.h>
#include <sgrn/sdk/Domains.hpp>
#include <sgrn/sdk/types.hpp>
#include <sgrn/utils/threading.hpp>
#include <httplib.h>
#include <rapidjson/document.h>
#include <stdexcept>
#include <string>

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

namespace sgrn::sdk
{

enum class AuthMode {
    AutomatedService,
    UserPassword,
    SessionToken,
};

// ─── Configuration ───────────────────────────────────────────────────────────

struct AutomatedServiceConfig {
    std::string backend_url_;
    AuthMode auth_mode_{AuthMode::AutomatedService};

    std::string public_token_;
    std::string private_token_;
    std::string token_;
    std::string secret_;

    std::string email_;
    std::string password_;
    std::string session_token_;

    std::string telemetry_path_{"/api/v1/telemetry/ingest"};
    bool compress_zstd_{true};
    uint8_t zstd_level_{5};
    std::size_t telemetry_max_queue_{10'000};
    double telemetry_timeout_s_{10.0};
    bool telemetry_block_on_full_{false};
    uint32_t telemetry_enqueue_timeout_ms_{1000};
    std::string telemetry_queue_dir_{"./telemetry-queue"};

    std::string user_storage_path_{"/api/v1/storage/files"};
    std::string service_storage_path_{"/api/v1/storage/automated-service/files"};
    std::string storage_path_{"/api/v1/storage/automated-service/files"};
    bool auto_decompress_zstd_{false};
    double storage_timeout_s_{30.0};

    bool retry_on_unauthorized_{true};
    uint8_t max_unauthorized_retries_{1};
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

/// Drive-API base path, routed by auth mode exactly like resolveStoragePath:
/// user sessions speak /api/v1/storage/drive/*, service sessions the
/// automated-service/* twins. SessionToken keeps the legacy service base.
inline std::string resolveDriveBase(const AutomatedServiceConfig& t_config) {
    if (t_config.auth_mode_ == AuthMode::UserPassword)
        return "/api/v1/storage/drive";
    return "/api/v1/storage/automated-service/drive";
}

inline std::string resolveStoragePath(const AutomatedServiceConfig& t_config) {
    if (!t_config.storage_path_.empty() && t_config.storage_path_ != "/api/v1/storage/automated-service/files") {
        return t_config.storage_path_;
    }
    switch (t_config.auth_mode_) {
        case AuthMode::UserPassword:
            return t_config.user_storage_path_.empty() ? "/api/v1/storage/files" : t_config.user_storage_path_;
        case AuthMode::SessionToken:
        case AuthMode::AutomatedService:
        default:
            return t_config.service_storage_path_.empty() ? "/api/v1/storage/automated-service/files" : t_config.service_storage_path_;
    }
}

inline std::string_view effectivePublicToken(const AutomatedServiceConfig& t_config) {
    return !t_config.public_token_.empty() ? std::string_view{t_config.public_token_} : std::string_view{t_config.token_};
}

inline std::string_view effectivePrivateToken(const AutomatedServiceConfig& t_config) {
    return !t_config.private_token_.empty() ? std::string_view{t_config.private_token_} : std::string_view{t_config.secret_};
}

} // namespace detail

using SgrnClientConfig = AutomatedServiceConfig;

// Forward declarations
class StorageClient;
class TelemetryClient;

/**
 * @brief SgrnClient
 * The unified asynchronous hub for accessing SGRN infrastructure.
 */
class SgrnClient {
public:
    explicit SgrnClient(SgrnClientConfig t_config);
    ~SgrnClient();

    /**
     * @brief Access domain-specific clients (lightweight views).
     */
    StorageClient& storage();
    TelemetryClient& telemetry();

    /**
     * @brief Unified asynchronous task publishing.
     */
    void publishTelemetryAsync(const std::string& t_object_name, const rapidjson::Value& t_data);
    void publishJsonTelemetryAsync(const rapidjson::Value& t_data);
    void uploadFileAsync(const std::string& t_remote_path, const std::string& t_local_path);

    /**
     * @brief Generic data table query (Synchronous).
     */
    rapidjson::Document query(const std::string& t_table, const std::string& t_params = "");

    /**
     * @brief Account-level listings backing domain navigation (Synchronous).
     * Mirror the dashboard's QueryList fetchers; empty on transport failure.
     */
    std::vector<IdNamePair> listOrganisations();
    std::vector<IdNamePair> listDomains(const std::string& t_organisation);
    /// Error-reporting variants (empty vector is ambiguous, see tryListDrive).
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListOrganisations();
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListDomains(const std::string& t_organisation);
    /// Statuses valid for user registration, per organisation.
    sgrn::Result<std::vector<IdNamePair>, std::string> tryListStatuses(const std::string& t_organisation);

    /// Account admin (mirrors the dashboard admin page; all admin-gated).
    sgrn::Result<std::vector<AdminUserEntry>, std::string> tryListUsers();
    sgrn::Result<std::string, std::string> registerUser(const NewUser& t_user);
    sgrn::Result<std::vector<ServiceEntry>, std::string> tryListServices();
    sgrn::Result<ServiceCredentials, std::string> registerService(const NewService& t_service);
    sgrn::Result<ServiceCredentials, std::string> rotateServiceToken(int64_t t_service_id);

    /// Session: sign out (revokes server-side) and password change.
    bool signOut();
    sgrn::Result<std::string, std::string> updatePassword(const std::string& t_old_password, const std::string& t_new_password);

    /// Raw JSON passthroughs for profile display (shapes stay server-owned).
    sgrn::Result<std::string, std::string> userInfoJson();
    sgrn::Result<std::string, std::string> storageStatsJson();
    sgrn::Result<std::string, std::string> storageConstraintsJson();

    const SgrnClientConfig& config() const {
        return config_;
    }

    // ── Internal implementation helpers (used by Domain views) ─────────────────
    bool signIn();
    bool hasSessionToken() const;
    std::string getSessionToken() const;
    void setSessionToken(std::string t_token);
    void clearSessionToken();

    rapidjson::Document makeRequest(const std::string& t_method, const std::string& t_url, const std::string& t_body = "");
    rapidjson::Document makeRequest(
        const std::string& t_method, const std::string& t_url, const std::string& t_body, const std::string& t_content_type);

    struct DownloadResult {
        bool ok{false};
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
    /// Same as listDrive(), but reports transport failures instead of
    /// collapsing them onto an empty listing (needed by interactive tools).
    sgrn::Result<DriveListing, std::string> tryListDrive(const std::string& t_path = "/", StorageScope t_scope = StorageScope::Auto);
    bool createDriveDirectory(const std::string& t_path, StorageScope t_scope = StorageScope::Auto);
    bool moveDriveItem(int64_t t_id, DriveItemType t_type, const std::string& t_new_name = "",
        std::optional<int64_t> t_target_parent_id = std::nullopt, StorageScope t_scope = StorageScope::Auto);
    bool deleteDriveItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope = StorageScope::Auto);

private:
    // ── Auth ───────────────────────────────────────
    bool signInAutomatedService();
    bool signInUser();
    bool signInSessionToken();

    // ── Background Worker ──────────────────────────────────────────────────────
    enum class TaskType { PublishTelemetry, UploadFile };
    struct Task {
        TaskType type;
        std::string identifier; // object_name or remote_path
        std::string data;       // JSON or file bytes
        uint8_t retries{0};
    };

    void processTask(const Task& t_task);
    void sendTelemetryTask(const std::string& t_json);

    // ── State ─────────────────────────────────────────────────────────────────
    std::unique_ptr<StorageClient> storage_client_;
    std::unique_ptr<TelemetryClient> telemetry_client_;

    SgrnClientConfig config_;

    mutable std::mutex session_token_mu_;
    std::string session_token_;

    std::unique_ptr<sgrn::utils::DynamicThreadPool> pool_;

    std::unique_ptr<httplib::Client> http_client_;
};

using SpaceClient = SgrnClient;
using AutomatedService = SgrnClient;

} // namespace sgrn::sdk

template <>
struct fmt::formatter<sgrn::sdk::AutomatedServiceConfig> : formatter<std::string_view> {
    auto format(const sgrn::sdk::AutomatedServiceConfig& t_cfg, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("AutomatedServiceConfig{{url=\"{}\", auth_mode={}, public_token=\"{}\"}}", t_cfg.backend_url_,
                sgrn::sdk::detail::authModeToString(t_cfg.auth_mode_), t_cfg.public_token_),
            t_ctx);
    }
};
