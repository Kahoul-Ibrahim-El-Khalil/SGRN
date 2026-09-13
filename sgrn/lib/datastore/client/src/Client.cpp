#include <sgrn/datastore/client/Client.hpp>
#include <sgrn/datastore/client/StorageClient.hpp>
#include <sgrn/datastore/client/Types.hpp>

#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/json.hpp>
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
    telemetry_client_ = std::make_unique<TelemetryClient>(*this);

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
TelemetryClient& DatastoreClient::telemetry() {
    return *telemetry_client_;
}

// ─── Asynchronous API ────────────────────────────────────────────────────────

void DatastoreClient::publishTelemetryAsync(const std::string& t_object_name, const rapidjson::Value& t_data) {
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

void DatastoreClient::publishJsonTelemetryAsync(const rapidjson::Value& t_data) {
    Task task;
    task.type = TaskType::PublishTelemetry;
    task.data = sgrn::utils::json::serializeCompact(t_data);

    pool_->post([this, t = std::move(task)] { processTask(t); });
}

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
        if (t_task.type == TaskType::PublishTelemetry) {
            sendTelemetryTask(t_task.data);
        } else if (t_task.type == TaskType::UploadFile) {
            doUpload(t_task.identifier, t_task.data);
        }
    } catch (const std::exception& e) {
        SGRN_WARN("DatastoreClient", "Task failed: {}", e.what());
    }
}

void DatastoreClient::sendTelemetryTask(const std::string& t_json) {
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

    if (res && res->status == 401 && config_.retry_on_unauthorized_) {
        clearSessionToken();
        if (signIn() && hasSessionToken()) {
            headers.erase("Authorization");
            headers.emplace("Authorization", "Bearer " + getSessionToken());
            res = http_client_->Post(config_.telemetry_path_, headers, body, "application/json");
        }
    }

    if (!res || res->status >= 400) {
        SGRN_WARN("DatastoreClient", "Failed to send telemetry: status={}", res ? res->status : 0);
    }
}

// ─── Synchronous Operations (Helpers) ────────────────────────────────────────

rapidjson::Document DatastoreClient::query(const std::string& t_table, const std::string& t_params) {
    std::string endpoint = "/api/v1/postgrest/automated-service/storage/files";
    if (t_table == "telemetry_data" || t_table == "telemetry") {
        endpoint = "/api/v1/postgrest/automated-service/telemetry/data";
    } else if (t_table == "telemetry_objects") {
        endpoint = "/api/v1/postgrest/automated-service/telemetry/objects";
    }

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
    auto parsed = sgrn::utils::json::deserialize(t_body);
    if (!parsed.hasError() && parsed.value().IsObject() && parsed.value().HasMember("error") && parsed.value()["error"].IsString()) {
        detail = parsed.value()["error"].GetString();
    } else {
        detail = t_body.size() > 300 ? t_body.substr(0, 300) + "…" : t_body;
    }
    if (detail.empty())
        detail = "(empty response body)";
    return fmt::format("HTTP {}: {}", t_status, detail);
}

std::vector<IdNamePair> parseIdNamePairs(const rapidjson::Document& t_doc) {
    std::vector<IdNamePair> out;
    if (!t_doc.IsArray())
        return out;
    for (const auto& e : t_doc.GetArray()) {
        if (!e.IsObject())
            continue;
        IdNamePair p;
        if (e.HasMember("id")) {
            const auto& id = e["id"];
            if (id.IsString())
                p.id_ = id.GetString();
            else if (id.IsInt64())
                p.id_ = std::to_string(id.GetInt64());
            else if (id.IsUint64())
                p.id_ = std::to_string(id.GetUint64());
        }
        if (e.HasMember("name") && e["name"].IsString())
            p.name_ = e["name"].GetString();
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
    if (doc.IsNull())
        return sgrn::Result<std::vector<IdNamePair>, std::string>::Error("list organisations failed: empty response");
    return parseIdNamePairs(doc);
}

std::vector<IdNamePair> DatastoreClient::listDomains() {
    auto r = tryListDomains();
    return r.hasError() ? std::vector<IdNamePair>{} : r.value();
}

sgrn::Result<std::vector<IdNamePair>, std::string> DatastoreClient::tryListDomains() {
    auto doc = makeRequest("GET", "/api/v1/domains");
    if (doc.IsNull())
        return sgrn::Result<std::vector<IdNamePair>, std::string>::Error("list domains failed: empty response (check session)");
    return parseIdNamePairs(doc);
}

namespace
{

std::string strField(const rapidjson::Value& t_obj, const char* t_key) {
    return (t_obj.HasMember(t_key) && t_obj[t_key].IsString()) ? t_obj[t_key].GetString() : std::string{};
}

// Integer-or-string id tolerant parse (generated CRUD rows stringify ids).
int64_t parseIntField(const rapidjson::Value& t_val) {
    if (t_val.IsInt64())
        return t_val.GetInt64();
    if (t_val.IsUint64())
        return static_cast<int64_t>(t_val.GetUint64());
    if (t_val.IsInt())
        return t_val.GetInt();
    if (t_val.IsUint())
        return t_val.GetUint();
    if (t_val.IsString()) {
        try {
            size_t pos = 0;
            const long long v = std::stoll(t_val.GetString(), &pos);
            if (pos == std::string(t_val.GetString()).size())
                return static_cast<int64_t>(v);
        } catch (...) {
        }
    }
    return 0;
}

sgrn::Result<rapidjson::Document, std::string> postJson(
    DatastoreClient& t_client, const std::string& t_endpoint, rapidjson::Document t_body) {
    auto doc = t_client.makeRequest("POST", t_endpoint, sgrn::utils::json::serializeCompact(t_body));
    if (doc.IsNull())
        return sgrn::Result<rapidjson::Document, std::string>::Error("request failed: empty response from " + t_endpoint);
    return std::move(doc);
}

bool responseOk(const rapidjson::Document& t_doc) {
    return t_doc.IsObject() && ((!t_doc.HasMember("success")) || (t_doc["success"].IsBool() && t_doc["success"].GetBool())) &&
           !t_doc.HasMember("error");
}

std::string responseMessage(const rapidjson::Document& t_doc, const std::string& t_fallback) {
    if (t_doc.IsObject()) {
        if (t_doc.HasMember("message") && t_doc["message"].IsString())
            return t_doc["message"].GetString();
        if (t_doc.HasMember("error") && t_doc["error"].IsString())
            return t_doc["error"].GetString();
    }
    return t_fallback;
}

} // namespace

sgrn::Result<std::vector<IdNamePair>, std::string> DatastoreClient::tryListStatuses(const std::string& t_organisation) {
    auto doc = makeRequest("GET", "/api/v1/query/statuses?organisation=" + urlEncodeQueryValue(t_organisation));
    if (doc.IsNull())
        return sgrn::Result<std::vector<IdNamePair>, std::string>::Error("list statuses failed: empty response");
    return parseIdNamePairs(doc);
}

sgrn::Result<std::vector<AdminUserEntry>, std::string> DatastoreClient::tryListUsers() {
    auto doc = makeRequest("GET", "/api/v1/admin/users");
    if (doc.IsNull())
        return sgrn::Result<std::vector<AdminUserEntry>, std::string>::Error("list users failed: empty response (admin only?)");
    if (!doc.IsArray())
        return sgrn::Result<std::vector<AdminUserEntry>, std::string>::Error("list users failed: unexpected response shape");
    std::vector<AdminUserEntry> out;
    for (const auto& e : doc.GetArray()) {
        if (!e.IsObject())
            continue;
        AdminUserEntry u;
        u.id_ = e.HasMember("id") && e["id"].IsInt64() ? e["id"].GetInt64() : 0;
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
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    auto add = [&](const char* t_key, const std::string& t_val) {
        body.AddMember(rapidjson::Value(t_key, alloc), rapidjson::Value(t_val.c_str(), alloc), alloc);
    };
    add("first_name", t_user.first_name_);
    add("family_name", t_user.family_name_);
    add("email", t_user.email_);
    add("password", t_user.password_);
    add("phone_number", t_user.phone_number_);
    add("organisation", t_user.organisation_);
    add("status", t_user.status_);
    add("domain", t_user.domain_);
    auto r = postJson(*this, "/api/v1/admin/users/register", std::move(body));
    if (r.hasError())
        return sgrn::Result<std::string, std::string>::Error(r.error());
    if (!responseOk(r.value()))
        return sgrn::Result<std::string, std::string>::Error(responseMessage(r.value(), "registration failed"));
    return responseMessage(r.value(), "registered");
}

sgrn::Result<std::vector<ServiceEntry>, std::string> DatastoreClient::tryListServices() {
    auto doc = makeRequest("GET", "/api/v1/automated-services");
    if (doc.IsNull())
        return sgrn::Result<std::vector<ServiceEntry>, std::string>::Error("list services failed: empty response (admin only?)");
    if (!doc.IsArray())
        return sgrn::Result<std::vector<ServiceEntry>, std::string>::Error("list services failed: unexpected response shape");
    std::vector<ServiceEntry> out;
    for (const auto& e : doc.GetArray()) {
        if (!e.IsObject())
            continue;
        // Generated CRUD rows carry every value as a string and expose no
        // computed columns: accept numeric-or-string ids and derive
        // is_active from status here.
        ServiceEntry s;
        s.id_ = e.HasMember("id") ? parseIntField(e["id"]) : 0;
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
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    auto add = [&](const char* t_key, const std::string& t_val) {
        body.AddMember(rapidjson::Value(t_key, alloc), rapidjson::Value(t_val.c_str(), alloc), alloc);
    };
    add("name", t_service.name_);
    add("organisation", t_service.organisation_);
    add("domain", t_service.domain_);
    if (!t_service.kind_.empty()) {
        rapidjson::Value meta(rapidjson::kObjectType);
        meta.AddMember("kind", rapidjson::Value(t_service.kind_.c_str(), alloc), alloc);
        body.AddMember("metadata", meta, alloc);
    } else {
        body.AddMember("metadata", rapidjson::Value(rapidjson::kObjectType), alloc);
    }
    auto r = postJson(*this, "/api/v1/admin/automated-services/register", std::move(body));
    if (r.hasError())
        return sgrn::Result<ServiceCredentials, std::string>::Error(r.error());
    if (!responseOk(r.value()))
        return sgrn::Result<ServiceCredentials, std::string>::Error(responseMessage(r.value(), "registration failed"));
    ServiceCredentials creds;
    creds.message_ = responseMessage(r.value(), "registered");
    creds.token_ = strField(r.value(), "token");
    creds.token_secret_ = strField(r.value(), "token_secret");
    return creds;
}

sgrn::Result<ServiceCredentials, std::string> DatastoreClient::rotateServiceToken(int64_t t_service_id) {
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    body.AddMember("automated_service_id", t_service_id, alloc);
    auto r = postJson(*this, "/api/v1/admin/automated-services/rotate-token", std::move(body));
    if (r.hasError())
        return sgrn::Result<ServiceCredentials, std::string>::Error(r.error());
    if (!responseOk(r.value()))
        return sgrn::Result<ServiceCredentials, std::string>::Error(responseMessage(r.value(), "rotation failed"));
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
    if (doc.IsNull())
        return false;
    clearSessionToken();
    return true;
}

sgrn::Result<std::string, std::string> DatastoreClient::updatePassword(
    const std::string& t_old_password, const std::string& t_new_password) {
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    body.AddMember("old_password", rapidjson::Value(t_old_password.c_str(), alloc), alloc);
    body.AddMember("new_password", rapidjson::Value(t_new_password.c_str(), alloc), alloc);
    auto r = postJson(*this, "/api/v1/auth/user/password", std::move(body));
    if (r.hasError())
        return sgrn::Result<std::string, std::string>::Error(r.error());
    if (!responseOk(r.value()))
        return sgrn::Result<std::string, std::string>::Error(responseMessage(r.value(), "password update failed"));
    return responseMessage(r.value(), "password updated");
}

sgrn::Result<std::string, std::string> DatastoreClient::userInfoJson() {
    auto doc = makeRequest("GET", "/api/v1/query/user/info");
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("user info failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageStatsJson() {
    auto doc = makeRequest("GET", "/api/v1/storage/stats");
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("stats failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageConstraintsJson() {
    auto doc = makeRequest("GET", "/api/v1/storage/info");
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("constraints failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminOverviewJson(uint32_t t_max_pages) {
    auto doc = makeRequest("GET", "/api/v1/admin/storage/overview?max_pages=" + std::to_string(t_max_pages));
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("storage overview failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminOrphansJson(
    const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit, uint32_t t_max_pages) {
    std::string url = "/api/v1/admin/storage/orphans?limit=" + std::to_string(t_limit) + "&max_pages=" + std::to_string(t_max_pages);
    if (!t_bucket.empty())
        url += "&bucket=" + urlEncodeQueryValue(t_bucket);
    if (!t_prefix.empty())
        url += "&prefix=" + urlEncodeQueryValue(t_prefix);
    auto doc = makeRequest("GET", url);
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("storage orphans failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

sgrn::Result<std::string, std::string> DatastoreClient::storageAdminPurgeOrphansJson(
    const std::string& t_bucket, const std::vector<std::string>& t_keys, const std::string& t_prefix, bool t_dry_run, uint32_t t_limit) {
    rapidjson::Document body;
    auto& alloc = body.GetAllocator();
    body.SetObject();
    if (!t_bucket.empty())
        body.AddMember("bucket", rapidjson::Value(t_bucket.c_str(), alloc), alloc);
    if (!t_keys.empty()) {
        rapidjson::Value keys(rapidjson::kArrayType);
        for (const auto& k : t_keys)
            keys.PushBack(rapidjson::Value(k.c_str(), alloc), alloc);
        body.AddMember("keys", keys, alloc);
    }
    if (!t_prefix.empty())
        body.AddMember("prefix", rapidjson::Value(t_prefix.c_str(), alloc), alloc);
    body.AddMember("dry_run", t_dry_run, alloc);
    body.AddMember("limit", t_limit, alloc);
    auto doc = makeRequest("POST", "/api/v1/admin/storage/orphans/purge", sgrn::utils::json::serializeCompact(body));
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("storage purge failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
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
    if (doc.IsNull())
        return sgrn::Result<std::string, std::string>::Error("storage search failed: empty response");
    return sgrn::utils::json::serializeCompact(doc);
}

namespace
{

// Parse a raw admin-storage JSON body into a rapidjson Document.
sgrn::Result<rapidjson::Document, std::string> parseAdminStorageJson(sgrn::Result<std::string, std::string> t_raw, const char* t_what) {
    if (t_raw.hasError())
        return sgrn::Result<rapidjson::Document, std::string>::Error(t_raw.error());
    auto parsed = sgrn::utils::json::deserialize(t_raw.value());
    if (parsed.hasError())
        return sgrn::Result<rapidjson::Document, std::string>::Error(std::string(t_what) + ": invalid JSON response");
    return std::move(parsed.value());
}

bool jsonBool(const rapidjson::Value& t_obj, const char* t_key) {
    return t_obj.HasMember(t_key) && t_obj[t_key].IsBool() && t_obj[t_key].GetBool();
}

int64_t jsonInt(const rapidjson::Value& t_obj, const char* t_key) {
    if (!t_obj.HasMember(t_key))
        return 0;
    return parseIntField(t_obj[t_key]);
}

void fillOrphanKeys(const rapidjson::Value& t_doc, const char* t_key, std::vector<StorageOrphanKey>& t_out) {
    if (!t_doc.HasMember(t_key) || !t_doc[t_key].IsArray())
        return;
    for (const auto& e : t_doc[t_key].GetArray()) {
        if (!e.IsObject())
            continue;
        StorageOrphanKey k;
        k.key_ = strField(e, "key");
        k.size_ = e.HasMember("size") ? parseIntField(e["size"]) : 0;
        k.etag_ = strField(e, "etag");
        t_out.push_back(std::move(k));
    }
}

void fillStringList(const rapidjson::Value& t_doc, const char* t_key, std::vector<std::string>& t_out) {
    if (!t_doc.HasMember(t_key) || !t_doc[t_key].IsArray())
        return;
    for (const auto& e : t_doc[t_key].GetArray()) {
        if (e.IsString())
            t_out.emplace_back(e.GetString());
        else if (e.IsObject() && e.HasMember("key") && e["key"].IsString())
            t_out.emplace_back(e["key"].GetString());
    }
}

} // namespace

sgrn::Result<std::vector<StorageBucketCensus>, std::string> DatastoreClient::tryStorageAdminOverview(uint32_t t_max_pages) {
    auto doc_res = parseAdminStorageJson(storageAdminOverviewJson(t_max_pages), "storage overview");
    if (doc_res.hasError())
        return sgrn::Result<std::vector<StorageBucketCensus>, std::string>::Error(doc_res.error());
    rapidjson::Document doc = std::move(doc_res.value());
    if (!doc.IsObject() || !doc.HasMember("buckets") || !doc["buckets"].IsArray())
        return sgrn::Result<std::vector<StorageBucketCensus>, std::string>::Error("storage overview: unexpected response shape");
    std::vector<StorageBucketCensus> out;
    for (const auto& b : doc["buckets"].GetArray()) {
        if (!b.IsObject())
            continue;
        StorageBucketCensus c;
        c.name_ = strField(b, "name");
        if (b.HasMember("minio") && b["minio"].IsObject()) {
            c.minio_objects_ = jsonInt(b["minio"], "objects");
            c.minio_bytes_ = jsonInt(b["minio"], "bytes");
            c.minio_truncated_ = jsonBool(b["minio"], "truncated");
        }
        if (b.HasMember("db") && b["db"].IsObject()) {
            c.db_objects_ = jsonInt(b["db"], "objects");
            c.db_bytes_ = jsonInt(b["db"], "bytes");
            c.db_files_ = jsonInt(b["db"], "files");
        }
        out.push_back(std::move(c));
    }
    return out;
}

sgrn::Result<StorageOrphansReport, std::string> DatastoreClient::tryStorageAdminOrphans(
    const std::string& t_bucket, const std::string& t_prefix, uint32_t t_limit, uint32_t t_max_pages) {
    auto doc_res = parseAdminStorageJson(storageAdminOrphansJson(t_bucket, t_prefix, t_limit, t_max_pages), "storage orphans");
    if (doc_res.hasError())
        return sgrn::Result<StorageOrphansReport, std::string>::Error(doc_res.error());
    rapidjson::Document doc = std::move(doc_res.value());
    if (!doc.IsObject())
        return sgrn::Result<StorageOrphansReport, std::string>::Error("storage orphans: unexpected response shape");
    StorageOrphansReport r;
    r.bucket_ = strField(doc, "bucket");
    r.prefix_ = strField(doc, "prefix");
    r.minio_scanned_ = jsonInt(doc, "minio_scanned");
    r.minio_truncated_ = jsonBool(doc, "minio_truncated");
    fillOrphanKeys(doc, "minio_only", r.minio_only_);
    r.minio_only_count_ = jsonInt(doc, "minio_only_count");
    r.minio_only_bytes_ = jsonInt(doc, "minio_only_bytes");
    fillOrphanKeys(doc, "db_missing", r.db_missing_);
    r.db_missing_unchecked_ = jsonInt(doc, "db_missing_unchecked");
    return r;
}

sgrn::Result<StoragePurgeResult, std::string> DatastoreClient::tryStorageAdminPurge(
    const std::string& t_bucket, const std::vector<std::string>& t_keys, const std::string& t_prefix, bool t_dry_run, uint32_t t_limit) {
    auto doc_res = parseAdminStorageJson(storageAdminPurgeOrphansJson(t_bucket, t_keys, t_prefix, t_dry_run, t_limit), "storage purge");
    if (doc_res.hasError())
        return sgrn::Result<StoragePurgeResult, std::string>::Error(doc_res.error());
    rapidjson::Document doc = std::move(doc_res.value());
    if (!doc.IsObject())
        return sgrn::Result<StoragePurgeResult, std::string>::Error("storage purge: unexpected response shape");
    StoragePurgeResult r;
    r.dry_run_ = jsonBool(doc, "dry_run");
    if (r.dry_run_)
        fillStringList(doc, "would_delete", r.affected_);
    else
        fillStringList(doc, "deleted", r.affected_);
    fillStringList(doc, "errors", r.errors_);
    fillStringList(doc, "skipped", r.skipped_);
    return r;
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

rapidjson::Document DatastoreClient::makeRequest(const std::string& t_method, const std::string& t_url, const std::string& body) {
    return makeRequest(t_method, t_url, body, "application/json");
}

rapidjson::Document DatastoreClient::makeRequest(
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
            rapidjson::Document null_doc;
            null_doc.SetNull();
            return null_doc;
        }
    }

    if (res) {
        if (res->status >= 400) {
            SGRN_ERROR("DatastoreClient", "Request failed: {} {} -> status {}, body: {}", t_method, t_url, res->status, res->body);
        }
        auto root_opt = sgrn::utils::json::deserialize(res->body);
        if (!root_opt.hasError()) {
            return std::move(root_opt.value());
        }
    } else {
        SGRN_ERROR("DatastoreClient", "Request failed: {} {} -> No response", t_method, t_url);
    }
    rapidjson::Document null_doc;
    null_doc.SetNull();
    return null_doc;
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
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/zip?path=" + t_path +
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
    if (t_json.IsNull())
        return {};
    return sgrn::datastore::client::detail::parseDriveListing(t_json);
}

sgrn::Result<DriveListing, std::string> DatastoreClient::tryListDrive(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/list?path=" + t_path +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    auto t_json = makeRequest("GET", endpoint);
    if (t_json.IsNull())
        return sgrn::Result<DriveListing, std::string>::Error(
            fmt::format("list '{}' failed: empty response (check URL, network and session)", t_path));
    return sgrn::datastore::client::detail::parseDriveListing(t_json);
}

bool DatastoreClient::createDriveDirectory(const std::string& t_path, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) + "/mkdir?path=" + t_path +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("POST", endpoint).IsNull();
}

bool DatastoreClient::moveDriveItem(
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
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) +
                           "/move?type=" + sgrn::datastore::client::detail::driveItemTypeToString(t_type) + "&id=" + std::to_string(t_id) +
                           "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("PATCH", endpoint, sgrn::utils::json::serializeCompact(body)).IsNull();
}

bool DatastoreClient::deleteDriveItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope) {
    const auto actual_scope = (t_scope == StorageScope::Auto)
                                  ? (config_.auth_mode_ == AuthMode::UserPassword ? StorageScope::Users : StorageScope::AutomatedServices)
                                  : t_scope;
    std::string endpoint = sgrn::datastore::client::detail::resolveDriveBase(config_) +
                           "/delete?type=" + sgrn::datastore::client::detail::driveItemTypeToString(t_type) +
                           "&id=" + std::to_string(t_id) + "&scope=" + sgrn::datastore::client::detail::storageScopeToString(actual_scope);
    return !makeRequest("DELETE", endpoint).IsNull();
}

} // namespace sgrn::datastore::client
