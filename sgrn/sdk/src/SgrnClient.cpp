#include <sgrn/sdk/Domains.hpp>
#include <sgrn/sdk/SgrnClient.hpp>
#include <sgrn/sdk/types.hpp>

#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/mime.hpp>
#include <sgrn/utils/strings.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <filesystem>
#include <fstream>

namespace sgrn::sdk
{

static constexpr std::string_view kSignInServicePath = "/api/v1/auth/automated-service/signin";
static constexpr std::string_view kSignInUserPath = "/api/v1/auth/user/signin";
static constexpr std::string_view kDriveListPath = "/api/v1/storage/automated-service/drive/list";
static constexpr std::string_view kDriveMkdirPath = "/api/v1/storage/automated-service/drive/mkdir";
static constexpr std::string_view kDriveMovePath = "/api/v1/storage/automated-service/drive/move";
static constexpr std::string_view kDriveDeletePath = "/api/v1/storage/automated-service/drive/delete";
static constexpr std::string_view kDriveZipPath = "/api/v1/storage/automated-service/drive/zip";

SgrnClient::SgrnClient(SgrnClientConfig t_config)
    : config_(std::move(t_config)) {

    // Configure httplib client
    http_client_ = std::make_unique<httplib::Client>(config_.backend_url_);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    http_client_->enable_server_certificate_verification(false);
#endif

    http_client_->set_connection_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_read_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_write_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_keep_alive(true);

    // Initialize domain views
    storage_client_ = std::make_unique<StorageClient>(*this);
    telemetry_client_ = std::make_unique<TelemetryClient>(*this);

    // Initial sign-in if needed
    if (config_.auth_mode_ == AuthMode::SessionToken) {
        signInSessionToken();
    } else {
        signIn();
    }

    pool_ = std::make_unique<sgrn::utils::DynamicThreadPool>(2);

    SGRN_INFO("SgrnClient", "Initialized with backend: {}", config_.backend_url_);
}

SgrnClient::~SgrnClient() {
    pool_.reset();
}

StorageClient& SgrnClient::storage() {
    return *storage_client_;
}
TelemetryClient& SgrnClient::telemetry() {
    return *telemetry_client_;
}

// ─── Asynchronous API ────────────────────────────────────────────────────────

void SgrnClient::publishTelemetryAsync(const std::string& t_object_name, const rapidjson::Value& t_data) {
    rapidjson::Document payload;
    auto& alloc = payload.GetAllocator();
    payload.SetObject();
    payload.AddMember("object_name", rapidjson::Value(t_object_name.c_str(), alloc), alloc);

    rapidjson::Value data_copy;
    data_copy.CopyFrom(t_data, alloc);
    payload.AddMember("data", data_copy, alloc);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    payload.AddMember("timestamp", ms, alloc);

    Task task;
    task.type = TaskType::PublishTelemetry;
    task.data = sgrn::utils::json::serializeCompact(payload);

    pool_->post([this, t = std::move(task)] { processTask(t); });
}

void SgrnClient::publishJsonTelemetryAsync(const rapidjson::Value& t_data) {
    Task task;
    task.type = TaskType::PublishTelemetry;
    task.data = sgrn::utils::json::serializeCompact(t_data);

    pool_->post([this, t = std::move(task)] { processTask(t); });
}

void SgrnClient::uploadFileAsync(const std::string& t_remote_path, const std::string& t_local_path) {
    Task task;
    task.type = TaskType::UploadFile;
    task.identifier = t_remote_path;

    // Read file bytes
    std::ifstream ifs(t_local_path, std::ios::binary);
    if (!ifs) {
        SGRN_ERROR("SgrnClient", "Failed to open local file for async upload: {}", t_local_path);
        return;
    }
    std::string t_bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    task.data = std::move(t_bytes);

    pool_->post([this, t = std::move(task)] { processTask(t); });
}

// ─── Background Worker ──────────────────────────────────────────────────────

void SgrnClient::processTask(const Task& t_task) {
    try {
        if (t_task.type == TaskType::PublishTelemetry) {
            sendTelemetryTask(t_task.data);
        } else if (t_task.type == TaskType::UploadFile) {
            doUpload(t_task.identifier, t_task.data);
        }
    } catch (const std::exception& e) {
        SGRN_WARN("SgrnClient", "Task failed: {}", e.what());
    }
}

void SgrnClient::sendTelemetryTask(const std::string& t_json) {
    if (!hasSessionToken())
        signIn();

    std::string body = t_json;
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    if (config_.compress_zstd_) {
        auto compressed = sgrn::utils::compressStringZstd(body, config_.zstd_level_);
        if (compressed) {
            body = std::move(compressed.value());
            headers.emplace("Content-Encoding", "zstd");
        }
    }

    auto res = http_client_->Post(config_.telemetry_path_, headers, body, "application/json");

    // Retry once if unauthorized
    if (res && res->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res = http_client_->Post(config_.telemetry_path_, headers, body, "application/json");
        }
    }

    if (!res || res->status >= 400) {
        SGRN_WARN("SgrnClient", "Failed to send telemetry: status={}", res ? res->status : 0);
    }
}

// ─── Synchronous Operations (Helpers) ────────────────────────────────────────

rapidjson::Document SgrnClient::query(const std::string& t_table, const std::string& t_params) {
    std::string endpoint = "/api/v1/postgrest/automated-service/storage/files";
    if (t_table == "telemetry_data" || t_table == "telemetry") {
        endpoint = "/api/v1/postgrest/automated-service/telemetry/data";
    } else if (t_table == "telemetry_objects") {
        endpoint = "/api/v1/postgrest/automated-service/telemetry/objects";
    }

    std::string url = endpoint + (t_params.empty() ? "" : "?" + t_params);
    return makeRequest("GET", std::move(url));
}

bool SgrnClient::signIn() {
    switch (config_.auth_mode_) {
        case AuthMode::AutomatedService:
            return signInAutomatedService();
        case AuthMode::UserPassword:
            return signInUser();
        case AuthMode::SessionToken:
            return signInSessionToken();
        default:
            return false;
    }
}

bool SgrnClient::signInAutomatedService() {
    auto public_token = sgrn::sdk::detail::effectivePublicToken(config_);
    auto private_token = sgrn::sdk::detail::effectivePrivateToken(config_);

    if (public_token.empty() || private_token.empty())
        return false;

    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    body.AddMember("token", rapidjson::Value(public_token.data(), static_cast<rapidjson::SizeType>(public_token.size()), alloc), alloc);
    body.AddMember("secret", rapidjson::Value(private_token.data(), static_cast<rapidjson::SizeType>(private_token.size()), alloc), alloc);

    try {
        auto res = http_client_->Post(std::string(kSignInServicePath), sgrn::utils::json::serializeCompact(body), "application/json");
        if (res) {
            if (res->status == 200) {
                auto root_opt = sgrn::utils::json::deserialize(res->body);
                if (!root_opt.hasError() && root_opt.value().HasMember("token") && root_opt.value()["token"].IsString()) {
                    setSessionToken(root_opt.value()["token"].GetString());
                    return true;
                }
            } else {
                SGRN_ERROR("SgrnClient", "Sign-in failed with status {}: {}", res->status, res->body);
            }
        } else {
            SGRN_ERROR("SgrnClient", "Sign-in failed: No response from backend (check URL/Connectivity)");
        }
    } catch (const std::exception& e) {
        SGRN_ERROR("SgrnClient", "Sign-in exception: {}", e.what());
    }
    return false;
}

bool SgrnClient::signInUser() {
    if (config_.email_.empty() || config_.password_.empty())
        return false;

    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    body.AddMember("email", rapidjson::Value(config_.email_.c_str(), alloc), alloc);
    body.AddMember("password", rapidjson::Value(config_.password_.c_str(), alloc), alloc);

    try {
        auto res = http_client_->Post(std::string(kSignInUserPath), sgrn::utils::json::serializeCompact(body), "application/json");
        if (res && res->status == 200) {
            auto root_opt = sgrn::utils::json::deserialize(res->body);
            if (!root_opt.hasError() && root_opt.value().HasMember("token") && root_opt.value()["token"].IsString()) {
                setSessionToken(root_opt.value()["token"].GetString());
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

bool SgrnClient::signInSessionToken() {
    if (config_.session_token_.empty())
        return false;
    setSessionToken(config_.session_token_);
    return true;
}

bool SgrnClient::hasSessionToken() const {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    return !session_token_.empty();
}

std::string SgrnClient::getSessionToken() const {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    return session_token_;
}

void SgrnClient::setSessionToken(std::string t_token) {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    session_token_ = std::move(t_token);
}

void SgrnClient::clearSessionToken() {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    session_token_.clear();
}

rapidjson::Document SgrnClient::makeRequest(const std::string& t_method, const std::string& t_url, const std::string& body) {
    return makeRequest(t_method, t_url, body, "application/json");
}

rapidjson::Document SgrnClient::makeRequest(
    const std::string& t_method, const std::string& t_url, const std::string& body, const std::string& t_content_type) {
    if (!hasSessionToken())
        signIn();

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    httplib::Result res;
    if (t_method == "GET")
        res = http_client_->Get(t_url, headers);
    else if (t_method == "POST")
        res = http_client_->Post(t_url, headers, body, t_content_type);
    else if (t_method == "PATCH")
        res = http_client_->Patch(t_url, headers, body, t_content_type);
    else if (t_method == "DELETE")
        res = http_client_->Delete(t_url, headers);

    if (res && (res->status == 401 || res->status == 403) && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            // Re-auth succeeded: retry the original request with the new token.
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            if (t_method == "GET")
                res = http_client_->Get(t_url, headers);
            else if (t_method == "POST")
                res = http_client_->Post(t_url, headers, body, t_content_type);
            else if (t_method == "PATCH")
                res = http_client_->Patch(t_url, headers, body, t_content_type);
            else if (t_method == "DELETE")
                res = http_client_->Delete(t_url, headers);
        } else {
            SGRN_WARN("SgrnClient", "Re-auth failed — aborting retry for {} {}", t_method, t_url);
            rapidjson::Document null_doc;
            null_doc.SetNull();
            return null_doc;
        }
    }

    if (res && res->status < 400) {
        auto root_opt = sgrn::utils::json::deserialize(res->body);
        if (!root_opt.hasError()) {
            return std::move(root_opt.value());
        }
    } else if (res) {
        SGRN_ERROR("SgrnClient", "Request failed: {} {} -> status {}, body: {}", t_method, t_url, res->status, res->body);
    } else {
        SGRN_ERROR("SgrnClient", "Request failed: {} {} -> No response", t_method, t_url);
    }
    rapidjson::Document null_doc;
    null_doc.SetNull();
    return null_doc;
}

SgrnClient::UploadResult SgrnClient::doUpload(const std::string& t_remote_path, std::string t_bytes) {
    UploadResult res;
    if (!hasSessionToken())
        signIn();

    const std::string endpoint = sgrn::sdk::detail::resolveStoragePath(config_);
    httplib::UploadFormDataItems items;
    items.push_back({"file", t_bytes, std::filesystem::path(t_remote_path).filename().string(), "application/octet-stream"});
    items.push_back({"path", t_remote_path, "", ""});

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Post(endpoint, headers, items);

    // Retry once if unauthorized
    if (res_http && res_http->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res_http = http_client_->Get(endpoint, headers);
        }
    }

    if (res_http) {
        res.ok = (res_http->status == 200 || res_http->status == 201);
        res.http_status = res_http->status;
        if (res.ok) {
            sgrn::Result<rapidjson::Document, std::string> root_opt = sgrn::utils::json::deserialize(res_http->body);
            if (!root_opt.hasError()) {
                const auto& root = root_opt.value();
                if (root.HasMember("id")) {
                    if (root["id"].IsInt64()) {
                        res.file_id = root["id"].GetInt64();
                    } else if (root["id"].IsInt()) {
                        res.file_id = root["id"].GetInt();
                    }
                }
                if (root.HasMember("key") && root["key"].IsString()) {
                    res.key = root["key"].GetString();
                }
            }
        } else {
            res.message = std::move(res_http->body);
            SGRN_ERROR("SgrnClient", "Upload failed: {} -> status {}, body: {}", endpoint, res.http_status, res.message);
        }
    } else {
        SGRN_ERROR("SgrnClient", "Upload failed: {} -> No response", endpoint);
    }
    return res;
}

SgrnClient::DownloadResult SgrnClient::doDownload(const std::string& t_remote_path) {
    DownloadResult res;
    if (!hasSessionToken())
        signIn();

    std::string endpoint = sgrn::sdk::detail::resolveStoragePath(config_) + "?path=" + t_remote_path;
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Get(endpoint, headers);

    // Retry once if unauthorized
    if (res_http && res_http->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res_http = http_client_->Get(endpoint, headers);
        }
    }

    if (res_http) {
        res.ok = (res_http->status == 200);
        if (res.ok) {
            res.bytes = res_http->body;
            if (res_http->has_header("Content-Type"))
                res.content_type = res_http->get_header_value("Content-Type");
        }
    }
    return res;
}

SgrnClient::DownloadResult SgrnClient::doDownloadDriveZip(const std::string& t_path, StorageScope t_scope) {
    DownloadResult res;
    if (!hasSessionToken())
        signIn();

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint =
        std::string(kDriveZipPath) + "?path=" + t_path + "&scope=" + sgrn::sdk::detail::storageScopeToString(actual_scope);

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Get(endpoint, headers);

    // Retry once if unauthorized
    if (res_http && res_http->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res_http = http_client_->Get(endpoint, headers);
        }
    }

    if (res_http) {
        res.ok = (res_http->status == 200);
        if (res.ok) {
            res.bytes = res_http->body;
        } else {
            res.message = res_http->body;
        }
    }
    return res;
}

DriveListing SgrnClient::listDrive(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint =
        std::string(kDriveListPath) + "?path=" + t_path + "&scope=" + sgrn::sdk::detail::storageScopeToString(actual_scope);
    auto t_json = makeRequest("GET", endpoint);
    if (t_json.IsNull())
        return {};
    return sgrn::sdk::detail::parseDriveListing(t_json);
}

bool SgrnClient::createDriveDirectory(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint =
        std::string(kDriveMkdirPath) + "?path=" + t_path + "&scope=" + sgrn::sdk::detail::storageScopeToString(actual_scope);
    return !makeRequest("POST", endpoint).IsNull();
}

bool SgrnClient::moveDriveItem(
    int64_t t_id, DriveItemType t_type, const std::string& t_new_name, std::optional<int64_t> t_target_parent_id, StorageScope t_scope) {
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    if (!t_new_name.empty()) {
        body.AddMember("new_name", rapidjson::Value(t_new_name.c_str(), alloc), alloc);
    }
    if (t_target_parent_id.has_value()) {
        body.AddMember("parent_id", *t_target_parent_id, alloc);
    }

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = std::string(kDriveMovePath) + "?type=" + sgrn::sdk::detail::driveItemTypeToString(t_type) +
                           "&id=" + std::to_string(t_id) + "&scope=" + sgrn::sdk::detail::storageScopeToString(actual_scope);
    return !makeRequest("PATCH", endpoint, sgrn::utils::json::serializeCompact(body)).IsNull();
}

bool SgrnClient::deleteDriveItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = std::string(kDriveDeletePath) + "?type=" + sgrn::sdk::detail::driveItemTypeToString(t_type) +
                           "&id=" + std::to_string(t_id) + "&scope=" + sgrn::sdk::detail::storageScopeToString(actual_scope);
    return !makeRequest("DELETE", endpoint).IsNull();
}

} // namespace sgrn::sdk
