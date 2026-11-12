#include "client-json.hpp"
#include <sgrn/datastore/client/Client.hpp>
#include <sgrn/datastore/client/StorageClient.hpp>
#include <sgrn/datastore/client/Types.hpp>

#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/glaze.hpp>
#include <sgrn/utils/mime.hpp>
#include <sgrn/utils/strings.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <stdexcept>
#include <string>

#include <filesystem>
#include <fstream>

namespace sgrn::datastore::client
{

static constexpr std::string_view kSignInServicePath = "/api/v1/auth/automated-service/signin";
static constexpr std::string_view kSignInUserPath = "/api/v1/auth/user/signin";

DatastoreClient::DatastoreClient(DatastoreClientConfig t_config)
    : config_(std::move(t_config)) {

    http_client_ = std::make_unique<httplib::Client>(config_.backend_url_);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    http_client_->enable_server_certificate_verification(false);
#endif

    http_client_->set_connection_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_read_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_write_timeout(std::chrono::seconds(static_cast<long>(config_.storage_timeout_s_)));
    http_client_->set_keep_alive(true);

    storage_client_ = std::make_unique<StorageClient>(*this);

    if (config_.auth_mode_ == AuthMode::SessionToken) {
        signInSessionToken();
    } else {
        signIn();
    }

    pool_ = std::make_unique<sgrn::utils::DynamicThreadPool>(2);

    SGRN_INFO("DatastoreClient", "Initialized with backend: {}", config_.backend_url_);
}

DatastoreClient::~DatastoreClient() {
    pool_.reset();
}

StorageClient& DatastoreClient::storage() {
    return *storage_client_;
}

// ─── Asynchronous API ────────────────────────────────────────────────────────

void DatastoreClient::uploadFileAsync(const std::string& t_remote_path, const std::string& t_local_path) {
    Task task;
    task.type = TaskType::UploadFile;
    task.identifier = t_remote_path;

    std::ifstream ifs(t_local_path, std::ios::binary);
    if (!ifs) {
        SGRN_ERROR("DatastoreClient", "Failed to open local file for async upload: {}", t_local_path);
        return;
    }
    std::string t_bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    task.data = std::move(t_bytes);

    pool_->post([this, t = std::move(task)] { processTask(t); });
}

// ─── Background Worker ──────────────────────────────────────────────────────

void DatastoreClient::processTask(const Task& t_task) {
    try {
        if (t_task.type == TaskType::UploadFile) {
            doUpload(t_task.identifier, t_task.data);
        }
    } catch (const std::exception& e) {
        SGRN_WARN("DatastoreClient", "Task failed: {}", e.what());
    }
}

// ─── Synchronous Operations (Helpers) ────────────────────────────────────────

sgrn::json::Value DatastoreClient::query(const std::string& t_table, const std::string& t_params) {
    std::string endpoint = "/api/v1/postgrest/automated-service/storage/files";

    std::string url = endpoint + (t_params.empty() ? "" : "?" + t_params);
    return makeRequest("GET", std::move(url));
}

namespace
{

std::string urlEncodeQueryValue(const std::string& t_in) {
    std::string out;
    out.reserve(t_in.size());
    for (unsigned char c : t_in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

// Renders a failed HTTP response as "HTTP <status>: <server error>".
// Prefers the server's JSON {"error": ...} text; falls back to the raw
// body (capped) so binary payloads can't flood the terminal.
std::string httpErrorText(int t_status, const std::string& t_body) {
    std::string detail;
    auto parsed = sgrn::json::parseValue(t_body);
    if (!parsed.hasError()) {
        const auto& doc = parsed.value();
        if (doc.is_object() && doc.contains("error") && doc.at("error").is_string()) {
            detail = doc.at("error").get_string();
        }
    }
    if (detail.empty()) {
        detail = t_body.size() > 300 ? t_body.substr(0, 300) + "…" : t_body;
    }
    if (detail.empty())
        detail = "(empty response body)";
    return fmt::format("HTTP {}: {}", t_status, detail);
}

std::vector<IdNamePair> parseIdNamePairs(const sgrn::json::Value& t_val) {
    std::vector<IdNamePair> out;
    if (!t_val.is_array())
        return out;
    for (const auto& e : t_val.get_array()) {
        if (!e.is_object())
            continue;
        IdNamePair p;
        if (e.contains("id")) {
            const auto& id = e.at("id");
            if (id.is_string())
                p.id_ = id.get_string();
            else if (id.is_number())
                p.id_ = std::to_string(id.as<int64_t>());
        }
        if (e.contains("name") && e.at("name").is_string())
            p.name_ = e.at("name").get_string();
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace

std::vector<IdNamePair> DatastoreClient::listOrganisations() {
    auto r = tryListOrganisations();
    return r.hasError() ? std::vector<IdNamePair>{} : r.value();
}

sgrn::Result<std::vector<IdNamePair>, std::string> DatastoreClient::tryListOrganisations() {
    auto doc = makeRequest("GET", "/api/v1/query/organisations");
    RETURN_ERROR_IF(doc.is_null(), "list organisations failed: empty response");
    return parseIdNamePairs(doc);
}

std::vector<IdNamePair> DatastoreClient::listDomains() {
    auto r = tryListDomains();
    return r.hasError() ? std::vector<IdNamePair>{} : r.value();
}

sgrn::Result<std::vector<IdNamePair>, std::string> DatastoreClient::tryListDomains() {
    auto doc = makeRequest("GET", "/api/v1/domains");
    RETURN_ERROR_IF(doc.is_null(), "list domains failed: empty response (check session)");
    return parseIdNamePairs(doc);
}

namespace
{

std::string strField(const sgrn::json::Value& t_obj, std::string_view t_key) {
    return (t_obj.is_object() && t_obj.contains(t_key) && t_obj.at(t_key).is_string()) ? t_obj.at(t_key).get_string() : std::string{};
}

// Integer-or-string id tolerant parse (generated CRUD rows stringify ids).
int64_t parseIntField(const sgrn::json::Value& t_val) {
    if (t_val.is_number())
        return t_val.as<int64_t>();
    if (t_val.is_string()) {
        try {
            size_t pos = 0;
            const long long v = std::stoll(t_val.get_string(), &pos);
            if (pos == t_val.get_string().size())
                return static_cast<int64_t>(v);
        } catch (...) {
        }
    }
    return 0;
}

// Serialize a passthrough response compactly (the old serializeCompact path).
sgrn::Result<std::string, std::string> writeCompact(const sgrn::json::Value& t_val) {
    auto res = sgrn::json::write(t_val);
    RETURN_ERROR_IF(res.hasError(), res.error().what());
    return res.value();
}

sgrn::Result<sgrn::json::Value, std::string> postJson(DatastoreClient& t_client, const std::string& t_endpoint, const std::string& t_body) {
    auto doc = t_client.makeRequest("POST", t_endpoint, t_body);
    RETURN_ERROR_IF(doc.is_null(), "request failed: empty response from " + t_endpoint);
    return doc;
}

bool responseOk(const sgrn::json::Value& t_doc) {
    return t_doc.is_object() && (!t_doc.contains("success") || (t_doc.at("success").is_boolean() && t_doc.at("success").get_boolean())) &&
           !t_doc.contains("error");
}

std::string responseMessage(const sgrn::json::Value& t_doc, const std::string& t_fallback) {
    if (t_doc.is_object()) {
        if (t_doc.contains("message") && t_doc.at("message").is_string())
            return t_doc.at("message").get_string();
        if (t_doc.contains("error") && t_doc.at("error").is_string())
            return t_doc.at("error").get_string();
    }
    return t_fallback;
}

} // namespace

sgrn::Result<std::vector<IdNamePair>, std::string> DatastoreClient::tryListStatuses(const std::string& t_organisation) {
    auto doc = makeRequest("GET", "/api/v1/query/statuses?organisation=" + urlEncodeQueryValue(t_organisation));
    RETURN_ERROR_IF(doc.is_null(), "list statuses failed: empty response");
    return parseIdNamePairs(doc);
}

sgrn::Result<std::vector<AdminUserEntry>, std::string> DatastoreClient::tryListUsers() {
    auto doc = makeRequest("GET", "/api/v1/admin/users");
    RETURN_ERROR_IF(doc.is_null(), "list users failed: empty response (admin only?)");
    RETURN_ERROR_IF(!doc.is_array(), "list users failed: unexpected response shape");
    std::vector<AdminUserEntry> out;
    for (const auto& e : doc.get_array()) {
        if (!e.is_object())
            continue;
        AdminUserEntry u;
        u.id_ = e.contains("id") && e.at("id").is_number() ? e.at("id").as<int64_t>() : 0;
        u.email_ = strField(e, "email");
        u.first_name_ = strField(e, "first_name");
        u.family_name_ = strField(e, "family_name");
        u.domain_ = strField(e, "domain");
        u.status_ = strField(e, "status");
        out.push_back(std::move(u));
    }
    return out;
}

sgrn::Result<std::string, std::string> DatastoreClient::registerUser(const NewUser& t_user) {
    detail::NewUserJson body;
    body.first_name = t_user.first_name_;
    body.family_name = t_user.family_name_;
    body.email = t_user.email_;
    body.password = t_user.password_;
    body.phone_number = t_user.phone_number_;
    body.organisation = t_user.organisation_;
    body.status = t_user.status_;
    body.domain = t_user.domain_;
    auto r = postJson(*this, "/api/v1/admin/users/register", sgrn::json::write(body).value_or(std::string{}));
    PROPAGATE_ERROR_IF(r);
    RETURN_ERROR_IF(!responseOk(r.value()), responseMessage(r.value(), "registration failed"));
    return responseMessage(r.value(), "registered");
}

sgrn::Result<std::vector<ServiceEntry>, std::string> DatastoreClient::tryListServices() {
    auto doc = makeRequest("GET", "/api/v1/automated-services");
    RETURN_ERROR_IF(doc.is_null(), "list services failed: empty response (admin only?)");
    RETURN_ERROR_IF(!doc.is_array(), "list services failed: unexpected response shape");
    std::vector<ServiceEntry> out;
    for (const auto& e : doc.get_array()) {
        if (!e.is_object())
            continue;
        // Generated CRUD rows carry every value as a string and expose no
        // computed columns: accept numeric-or-string ids and derive
        // is_active from status here.
        ServiceEntry s;
        s.id_ = e.contains("id") ? parseIntField(e.at("id")) : 0;
        s.name_ = strField(e, "name");
        s.token_ = strField(e, "token");
        s.is_active_ = strField(e, "status") == "active";
        s.domain_ = strField(e, "domain");
        s.created_at_ = strField(e, "created_at");
        out.push_back(std::move(s));
    }
    return out;
}

sgrn::Result<ServiceCredentials, std::string> DatastoreClient::registerService(const NewService& t_service) {
    detail::ServiceRegistrationJson body;
    body.name = t_service.name_;
    body.organisation = t_service.organisation_;
    body.domain = t_service.domain_;
    if (!t_service.kind_.empty()) {
        body.metadata.kind = t_service.kind_;
    }
    auto r = postJson(*this, "/api/v1/admin/automated-services/register", sgrn::json::writeOmitNull(body).value_or(std::string{}));
    PROPAGATE_ERROR_IF(r);
    RETURN_ERROR_IF(!responseOk(r.value()), responseMessage(r.value(), "registration failed"));
    ServiceCredentials creds;
    creds.message_ = responseMessage(r.value(), "registered");
    creds.token_ = strField(r.value(), "token");
    creds.token_secret_ = strField(r.value(), "token_secret");
    return creds;
}

sgrn::Result<ServiceCredentials, std::string> DatastoreClient::rotateServiceToken(int64_t t_service_id) {
    detail::RotateServiceTokenJson body;
    body.automated_service_id = t_service_id;
    auto r = postJson(*this, "/api/v1/admin/automated-services/rotate-token", sgrn::json::write(body).value_or(std::string{}));
    PROPAGATE_ERROR_IF(r);
    RETURN_ERROR_IF(!responseOk(r.value()), responseMessage(r.value(), "rotation failed"));
    ServiceCredentials creds;
    creds.message_ = responseMessage(r.value(), "rotated");
    creds.token_ = strField(r.value(), "token");
    creds.token_secret_ = strField(r.value(), "token_secret");
    return creds;
}

bool DatastoreClient::signOut() {
    const std::string path =
        (config_.auth_mode_ == AuthMode::UserPassword) ? "/api/v1/auth/user/signout" : "/api/v1/auth/automated-service/signout";
    auto doc = makeRequest("POST", path);
    if (doc.is_null())
        return false;
    clearSessionToken();
    return true;
}

sgrn::Result<std::string, std::string> DatastoreClient::updatePassword(
    const std::string& t_old_password, const std::string& t_new_password) {
    detail::PasswordUpdateJson body;
    body.old_password = t_old_password;
    body.new_password = t_new_password;
    auto r = postJson(*this, "/api/v1/auth/user/password", sgrn::json::write(body).value_or(std::string{}));
    PROPAGATE_ERROR_IF(r);
    RETURN_ERROR_IF(!responseOk(r.value()), responseMessage(r.value(), "password update failed"));
    return responseMessage(r.value(), "password updated");
}

sgrn::Result<std::string, std::string> DatastoreClient::userInfoJson() {
    auto doc = makeRequest("GET", "/api/v1/query/user/info");
    RETURN_ERROR_IF(doc.is_null(), "user info failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageStatsJson() {
    auto doc = makeRequest("GET", "/api/v1/storage/stats");
    RETURN_ERROR_IF(doc.is_null(), "stats failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageConstraintsJson() {
    auto doc = makeRequest("GET", "/api/v1/storage/info");
    RETURN_ERROR_IF(doc.is_null(), "constraints failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminOverviewJson(uint32_t t_max_pages) {
    auto doc = makeRequest("GET", "/api/v1/admin/storage/overview?max_pages=" + std::to_string(t_max_pages));
    RETURN_ERROR_IF(doc.is_null(), "storage overview failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminOrphansJson(
    const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit, uint32_t t_max_pages) {
    std::string url = "/api/v1/admin/storage/orphans?limit=" + std::to_string(t_limit) + "&max_pages=" + std::to_string(t_max_pages);
    if (!t_bucket.empty())
        url += "&bucket=" + urlEncodeQueryValue(t_bucket);
    if (!t_prefix.empty())
        url += "&prefix=" + urlEncodeQueryValue(t_prefix);
    auto doc = makeRequest("GET", url);
    RETURN_ERROR_IF(doc.is_null(), "storage orphans failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminPurgeOrphansJson(
    const std::string& t_bucket, const std::vector<std::string>& t_keys, const std::string& t_prefix, bool t_dry_run, uint32_t t_limit) {
    detail::PurgeOrphansJson body;
    if (!t_bucket.empty())
        body.bucket = t_bucket;
    if (!t_keys.empty())
        body.keys = t_keys;
    if (!t_prefix.empty())
        body.prefix = t_prefix;
    body.dry_run = t_dry_run;
    body.limit = t_limit;
    auto doc = makeRequest("POST", "/api/v1/admin/storage/orphans/purge", sgrn::json::writeOmitNull(body).value_or(std::string{}));
    RETURN_ERROR_IF(doc.is_null(), "storage purge failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminSearchJson(
    const std::string& t_bucket, const std::string& t_key, const std::string& t_prefix, uint32_t t_limit) {
    std::string url = "/api/v1/admin/storage/search?limit=" + std::to_string(t_limit);
    if (!t_bucket.empty())
        url += "&bucket=" + urlEncodeQueryValue(t_bucket);
    if (!t_key.empty())
        url += "&key=" + urlEncodeQueryValue(t_key);
    else if (!t_prefix.empty())
        url += "&prefix=" + urlEncodeQueryValue(t_prefix);
    auto doc = makeRequest("GET", url);
    RETURN_ERROR_IF(doc.is_null(), "storage search failed: empty response");
    return writeCompact(doc);
}

namespace
{

// Parse a raw admin-storage JSON body into a dynamic value.
sgrn::Result<sgrn::json::Value, std::string> parseAdminStorageJson(sgrn::Result<std::string, std::string> t_raw, const char* t_what) {
    PROPAGATE_ERROR_IF(t_raw);
    auto parsed = sgrn::json::parseValue(t_raw.value());
    RETURN_ERROR_IF(parsed.hasError(), std::string(t_what) + ": invalid JSON response");
    return parsed.value();
}

} // namespace

sgrn::Result<std::vector<StorageBucketCensus>, std::string> DatastoreClient::tryStorageAdminOverview(uint32_t t_max_pages) {
    auto doc_res = parseAdminStorageJson(storageAdminOverviewJson(t_max_pages), "storage overview");
    PROPAGATE_ERROR_IF(doc_res);
    const sgrn::json::Value& doc = doc_res.value();
    if (!doc.is_object() || !doc.contains("buckets") || !doc.at("buckets").is_array())
        return ::sgrn::Error("storage overview: unexpected response shape");
    std::vector<StorageBucketCensus> out;
    for (const auto& b : doc.at("buckets").get_array()) {
        if (!b.is_object())
            continue;
        auto c_res = sgrn::json::readFromValue<detail::StorageBucketCensusJson>(b);
        if (c_res.hasError())
            continue;
        out.push_back(detail::censusFromJson(c_res.value()));
    }
    return out;
}

sgrn::Result<StorageOrphansReport, std::string> DatastoreClient::tryStorageAdminOrphans(
    const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit, uint32_t t_max_pages) {
    auto doc_res = parseAdminStorageJson(storageAdminOrphansJson(t_bucket, t_prefix, t_limit, t_max_pages), "storage orphans");
    PROPAGATE_ERROR_IF(doc_res);
    const sgrn::json::Value& doc = doc_res.value();
    auto w_res = sgrn::json::readFromValue<detail::StorageOrphansReportJson>(doc);
    RETURN_ERROR_IF(w_res.hasError(), "storage orphans: unexpected response shape");
    return detail::orphansReportFromJson(w_res.value());
}

sgrn::Result<StoragePurgeResult, std::string> DatastoreClient::tryStorageAdminPurge(
    const std::string& t_bucket, const std::vector<std::string>& t_keys, const std::string& t_prefix, bool t_dry_run, uint32_t t_limit) {
    auto doc_res = parseAdminStorageJson(storageAdminPurgeOrphansJson(t_bucket, t_keys, t_prefix, t_dry_run, t_limit), "storage purge");
    PROPAGATE_ERROR_IF(doc_res);
    const sgrn::json::Value& doc = doc_res.value();
    auto w_res = sgrn::json::readFromValue<detail::StoragePurgeResultJson>(doc);
    RETURN_ERROR_IF(w_res.hasError(), "storage purge: unexpected response shape");
    return detail::purgeResultFromJson(w_res.value());
}

bool DatastoreClient::signIn() {
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

bool DatastoreClient::signInAutomatedService() {
    auto public_token = sgrn::datastore::client::detail::effectivePublicToken(config_);
    auto private_token = sgrn::datastore::client::detail::effectivePrivateToken(config_);

    if (public_token.empty() || private_token.empty())
        return false;

    detail::SignInServiceJson body;
    body.token = std::string(public_token);
    body.secret = std::string(private_token);

    try {
        auto res = http_client_->Post(std::string(kSignInServicePath), sgrn::json::write(body).value_or(std::string{}), "application/json");
        if (res) {
            if (res->status == 200) {
                auto root_res = sgrn::json::parseValue(res->body);
                const auto& root = root_res.value();
                if (!root_res.hasError() && root.is_object() && root.contains("token") && root.at("token").is_string()) {
                    setSessionToken(root.at("token").get_string());
                    return true;
                }
            } else {
                SGRN_ERROR("DatastoreClient", "Sign-in failed with status {}: {}", res->status, res->body);
            }
        } else {
            SGRN_ERROR("DatastoreClient", "Sign-in failed: No response from backend (check URL/Connectivity)");
        }
    } catch (const std::exception& e) {
        SGRN_ERROR("DatastoreClient", "Sign-in exception: {}", e.what());
    }
    return false;
}

bool DatastoreClient::signInUser() {
    if (config_.email_.empty() || config_.password_.empty())
        return false;

    detail::SignInUserJson body;
    body.email = config_.email_;
    body.password = config_.password_;

    try {
        auto res = http_client_->Post(std::string(kSignInUserPath), sgrn::json::write(body).value_or(std::string{}), "application/json");
        if (res && res->status == 200) {
            auto root_res = sgrn::json::parseValue(res->body);
            const auto& root = root_res.value();
            if (!root_res.hasError() && root.is_object() && root.contains("token") && root.at("token").is_string()) {
                setSessionToken(root.at("token").get_string());
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

bool DatastoreClient::signInSessionToken() {
    if (config_.session_token_.empty())
        return false;
    setSessionToken(config_.session_token_);
    return true;
}

bool DatastoreClient::hasSessionToken() const {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    return !session_token_.empty();
}

std::string DatastoreClient::getSessionToken() const {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    return session_token_;
}

void DatastoreClient::setSessionToken(std::string t_token) {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    session_token_ = std::move(t_token);
}

void DatastoreClient::clearSessionToken() {
    std::lock_guard<std::mutex> lock(session_token_mu_);
    session_token_.clear();
}

sgrn::json::Value DatastoreClient::makeRequest(const std::string& t_method, const std::string& t_url, const std::string& body) {
    return makeRequest(t_method, t_url, body, "application/json");
}

sgrn::json::Value DatastoreClient::makeRequest(
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
            SGRN_WARN("DatastoreClient", "Re-auth failed — aborting retry for {} {}", t_method, t_url);
            return sgrn::json::Value{}; // null sentinel
        }
    }

    if (res) {
        if (res->status >= 400) {
            SGRN_ERROR("DatastoreClient", "Request failed: {} {} -> status {}, body: {}", t_method, t_url, res->status, res->body);
        }
        auto root_res = sgrn::json::parseValue(res->body);
        if (!root_res.hasError()) {
            return root_res.value();
        }
    } else {
        SGRN_ERROR("DatastoreClient", "Request failed: {} {} -> No response", t_method, t_url);
    }
    return sgrn::json::Value{}; // null sentinel
}

DatastoreClient::UploadResult DatastoreClient::doUpload(const std::string& t_remote_path, std::string t_bytes, StorageScope t_scope) {
    UploadResult res;
    if (!hasSessionToken())
        signIn();

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;

    const std::string scope_str = sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    const std::string endpoint = sgrn::datastore::client::detail::resolveStoragePath(config_) +
                                 "?path=" + urlEncodeQueryValue(t_remote_path) + "&scope=" + scope_str;
    httplib::UploadFormDataItems items;
    items.push_back({"file", t_bytes, std::filesystem::path(t_remote_path).filename().string(), "application/octet-stream"});
    items.push_back({"path", t_remote_path, "", ""});
    items.push_back({"scope", scope_str, "", ""});

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Post(endpoint, headers, items);

    if (res_http && res_http->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res_http = http_client_->Post(endpoint, headers, items);
        }
    }

    if (res_http) {
        res.ok = (res_http->status == 200 || res_http->status == 201);
        res.http_status = res_http->status;
        if (res.ok) {
            auto root_res = sgrn::json::parseValue(res_http->body);
            if (!root_res.hasError()) {
                const auto& root = root_res.value();
                if (root.is_object() && root.contains("id") && root.at("id").is_number()) {
                    res.file_id = root.at("id").as<int64_t>();
                }
                if (root.is_object() && root.contains("key") && root.at("key").is_string()) {
                    res.key = root.at("key").get_string();
                }
            }
        } else {
            res.message = httpErrorText(res_http->status, res_http->body);
            SGRN_ERROR("DatastoreClient", "Upload failed: {} -> status {}, body: {}", endpoint, res.http_status, res.message);
        }
    } else {
        res.message = "no response from server";
        SGRN_ERROR("DatastoreClient", "Upload failed: {} -> No response", endpoint);
    }
    return res;
}

DatastoreClient::DownloadResult DatastoreClient::doDownload(const std::string& t_remote_path, StorageScope t_scope) {
    DownloadResult res;
    if (!hasSessionToken())
        signIn();

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;

    std::string endpoint = sgrn::datastore::client::detail::resolveStoragePath(config_) + "?path=" + urlEncodeQueryValue(t_remote_path) +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Get(endpoint, headers);

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
        res.http_status = res_http->status;
        if (res.ok) {
            res.bytes = std::move(res_http->body);
            if (res_http->has_header("Content-Type"))
                res.content_type = res_http->get_header_value("Content-Type");

            const bool ends_with_zst = (t_remote_path.size() >= 4 && t_remote_path.rfind(".zst") == t_remote_path.size() - 4);
            const bool is_zstd_payload =
                (res.content_type == "application/zstd") ||
                (res_http->has_header("X-Compressed") && res_http->get_header_value("X-Compressed") == "true") ||
                (res.bytes.size() >= 4 && static_cast<unsigned char>(res.bytes[0]) == 0x28 &&
                    static_cast<unsigned char>(res.bytes[1]) == 0xB5 && static_cast<unsigned char>(res.bytes[2]) == 0x2F &&
                    static_cast<unsigned char>(res.bytes[3]) == 0xFD);

            if (is_zstd_payload && !ends_with_zst) {
                auto dec_res = sgrn::utils::decompressStringZstd(res.bytes);
                if (!dec_res.hasError()) {
                    res.bytes = std::move(dec_res.value());
                } else {
                    SGRN_WARN(
                        "DatastoreClient", "Failed client-side transparent decompression for '{}': {}", t_remote_path, dec_res.error());
                }
            }
        } else {
            res.message = httpErrorText(res_http->status, res_http->body);
        }
    } else {
        res.message = "no response from server";
    }
    return res;
}

DatastoreClient::DownloadResult DatastoreClient::doDownloadDriveZip(const std::string& t_path, StorageScope t_scope) {
    DownloadResult res;
    if (!hasSessionToken())
        signIn();

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/zip?path=" + urlEncodeQueryValue(t_path) +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);

    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());

    auto res_http = http_client_->Get(endpoint, headers);

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
        res.http_status = res_http->status;
        if (res.ok) {
            res.bytes = res_http->body;
        } else {
            res.message = httpErrorText(res_http->status, res_http->body);
        }
    } else {
        res.message = "no response from server";
    }
    return res;
}

DriveListing DatastoreClient::listDrive(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/list?path=" + t_path +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    auto t_json = makeRequest("GET", endpoint);
    if (t_json.is_null())
        return {};
    auto w_res = sgrn::json::readFromValue<detail::DriveListingJson>(t_json);
    return w_res.hasError() ? DriveListing{} : detail::listingFromJson(w_res.value());
}

sgrn::Result<DriveListing, std::string> DatastoreClient::tryListDrive(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/list?path=" + t_path +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    auto t_json = makeRequest("GET", endpoint);
    RETURN_ERROR_IF(t_json.is_null(), fmt::format("list '{}' failed: empty response (check URL, network and session)", t_path));
    auto w_res = sgrn::json::readFromValue<detail::DriveListingJson>(t_json);
    RETURN_ERROR_IF(w_res.hasError(), fmt::format("list '{}' failed: unexpected response shape", t_path));
    return detail::listingFromJson(w_res.value());
}

bool DatastoreClient::createDriveDirectory(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/mkdir?path=" + t_path +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("POST", endpoint).is_null();
}

bool DatastoreClient::moveDriveItem(
    int64_t t_id, DriveItemType t_type, const std::string& t_new_name, std::optional<int64_t> t_target_parent_id, StorageScope t_scope) {
    detail::MoveDriveItemJson body;
    if (!t_new_name.empty()) {
        body.new_name = t_new_name;
    }
    if (t_target_parent_id.has_value()) {
        body.parent_id = *t_target_parent_id;
    }

    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) +
                           "/move?type=" + sgrn::datastore::client::detail::driveItemTypeToString(t_type) + "&id=" + std::to_string(t_id) +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("PATCH", endpoint, sgrn::json::writeOmitNull(body).value_or(std::string{})).is_null();
}

bool DatastoreClient::deleteDriveItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) +
                           "/delete?type=" + sgrn::datastore::client::detail::driveItemTypeToString(t_type) +
                           "&id=" + std::to_string(t_id) + "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("DELETE", endpoint).is_null();
}

// ── Resumable upload session management ──────────────────────────────────────

sgrn::Result<DatastoreClient::ResumableUploadSession, std::string> DatastoreClient::initUploadSession(const std::string& t_filename,
    const std::string& t_target_path, int64_t t_total_size, int64_t t_chunk_size, const std::string& t_mime_type, bool t_encrypt) {
    detail::UploadInitJson body;
    body.filename = t_filename;
    body.target_path = t_target_path;
    body.mime_type = t_mime_type;
    body.total_size = t_total_size;
    body.chunk_size = t_chunk_size;
    body.encrypt = t_encrypt;
    auto doc = makeRequest("POST", "/api/v1/storage/upload/init", sgrn::json::write(body).value_or(std::string{}));
    RETURN_ERROR_IF(doc.is_null(), "init upload session failed: empty response");
    RETURN_ERROR_IF(doc.is_object() && doc.contains("error"),
        doc.is_object() && doc.at("error").is_string() ? doc.at("error").get_string() : "server error");
    auto w_res = sgrn::json::readFromValue<detail::ResumableUploadSessionJson>(doc);
    RETURN_ERROR_IF(w_res.hasError(), "init upload session failed: unexpected response shape");
    return detail::sessionFromJson(w_res.value());
}

sgrn::Result<bool, std::string> DatastoreClient::uploadChunk(
    const std::string& t_upload_id, int32_t t_chunk_index, const char* t_data, size_t t_size) {
    if (!hasSessionToken())
        signIn();

    const std::string endpoint =
        "/api/v1/storage/upload/chunk?upload_id=" + urlEncodeQueryValue(t_upload_id) + "&chunk_index=" + std::to_string(t_chunk_index);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());
    std::string payload(t_data, t_size);

    auto res = http_client_->Put(endpoint, headers, payload, "application/octet-stream");

    if (res && res->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res = http_client_->Put(endpoint, headers, payload, "application/octet-stream");
        }
    }

    RETURN_ERROR_IF(!res, "chunk upload failed: no response");
    RETURN_ERROR_IF(res->status < 200 || res->status >= 300, httpErrorText(res->status, res->body));
    return true;
}

sgrn::Result<DatastoreClient::ResumableUploadStatus, std::string> DatastoreClient::getUploadStatus(const std::string& t_upload_id) {
    auto doc = makeRequest("GET", "/api/v1/storage/upload/status?upload_id=" + urlEncodeQueryValue(t_upload_id));
    RETURN_ERROR_IF(doc.is_null(), "get upload status failed: empty response");
    RETURN_ERROR_IF(doc.is_object() && doc.contains("error"),
        doc.is_object() && doc.at("error").is_string() ? doc.at("error").get_string() : "server error");
    auto w_res = sgrn::json::readFromValue<detail::ResumableUploadStatusJson>(doc);
    RETURN_ERROR_IF(w_res.hasError(), "get upload status failed: unexpected response shape");
    return detail::uploadStatusFromJson(w_res.value());
}

sgrn::Result<std::string, std::string> DatastoreClient::completeUploadSession(const std::string& t_upload_id) {
    detail::UploadIdJson body;
    body.upload_id = t_upload_id;
    auto doc = makeRequest("POST", "/api/v1/storage/upload/complete", sgrn::json::write(body).value_or(std::string{}));
    RETURN_ERROR_IF(doc.is_null(), "complete upload failed: empty response");
    RETURN_ERROR_IF(doc.is_object() && doc.contains("error"),
        doc.is_object() && doc.at("error").is_string() ? doc.at("error").get_string() : "server error");
    if (doc.is_object() && doc.contains("message") && doc.at("message").is_string())
        return doc.at("message").get_string();
    return std::string("Upload completed");
}

sgrn::Result<std::string, std::string> DatastoreClient::abortUploadSession(const std::string& t_upload_id) {
    auto doc = makeRequest("DELETE", "/api/v1/storage/upload/abort?upload_id=" + urlEncodeQueryValue(t_upload_id));
    RETURN_ERROR_IF(doc.is_null(), "abort upload failed: empty response");
    RETURN_ERROR_IF(doc.is_object() && doc.contains("error"),
        doc.is_object() && doc.at("error").is_string() ? doc.at("error").get_string() : "server error");
    if (doc.is_object() && doc.contains("message") && doc.at("message").is_string())
        return doc.at("message").get_string();
    return std::string("Upload aborted");
}

// ── Live session listing ─────────────────────────────────────────────────────

sgrn::Result<std::string, std::string> DatastoreClient::listActiveSessionsJson() {
    auto doc = makeRequest("GET", "/api/v1/admin/metaprobe/sessions");
    RETURN_ERROR_IF(doc.is_null(), "list sessions failed: empty response");
    return writeCompact(doc);
}

// ── Webhook management ───────────────────────────────────────────────────────

sgrn::Result<std::string, std::string> DatastoreClient::listWebhooksJson() {
    auto doc = makeRequest("GET", "/api/v1/admin/webhooks");
    RETURN_ERROR_IF(doc.is_null(), "list webhooks failed: empty response");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::registerWebhookJson(const std::string& t_url, const std::string& t_secret) {
    detail::RegisterWebhookJson body;
    body.url = t_url;
    if (!t_secret.empty())
        body.secret = t_secret;
    auto doc = makeRequest("POST", "/api/v1/admin/webhooks", sgrn::json::writeOmitNull(body).value_or(std::string{}));
    RETURN_ERROR_IF(doc.is_null(), "register webhook failed: empty response");
    RETURN_ERROR_IF(doc.is_object() && doc.contains("error"),
        doc.is_object() && doc.at("error").is_string() ? doc.at("error").get_string() : "server error");
    return writeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::deleteWebhookJson(int32_t t_webhook_id) {
    if (!hasSessionToken())
        signIn();
    const std::string endpoint = "/api/v1/admin/webhooks/" + std::to_string(t_webhook_id);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + getSessionToken());
    auto res = http_client_->Delete(endpoint, headers);
    if (res && res->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res = http_client_->Delete(endpoint, headers);
        }
    }
    RETURN_ERROR_IF(!res, "delete webhook failed: no response");
    RETURN_ERROR_IF(res->status < 200 || res->status >= 300, httpErrorText(res->status, res->body));
    auto root_res = sgrn::json::parseValue(res->body);
    if (!root_res.hasError())
        return writeCompact(root_res.value());
    return std::string("Webhook deleted");
}

} // namespace sgrn::datastore::client
