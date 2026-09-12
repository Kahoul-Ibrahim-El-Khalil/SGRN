#include <fmt/core.h>
#include <sgrn/datastore/client/Client.hpp>
#include <sgrn/datastore/client/StorageClient.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sgrn::datastore::client
{

namespace
{

// Precise local-filesystem failure text (parent missing vs permission vs …).
std::string localWriteError(const std::string& t_local_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path parent = fs::path(t_local_path).parent_path();
    if (!parent.empty() && !fs::exists(parent, ec))
        return fmt::format("cannot write '{}': parent directory does not exist", t_local_path);
    return fmt::format("cannot write '{}': {}", t_local_path, std::strerror(errno));
}

} // namespace

// ─── Storage Client ──────────────────────────────────────────────────────────

StorageClient::StorageClient(DatastoreClient& t_client)
    : client_(t_client) {
}

void StorageClient::uploadAsync(const std::string& t_remote_path, const std::string& t_local_path) {
    client_.uploadFileAsync(t_remote_path, t_local_path);
}

void StorageClient::downloadAsync([[maybe_unused]] const std::string& t_remote_path, [[maybe_unused]] const std::string& t_local_path) {
}

TransferOutcome StorageClient::upload(const std::string& t_remote_path, const std::string& t_local_path, StorageScope t_scope) {
    std::ifstream ifs(t_local_path, std::ios::binary);
    if (!ifs)
        return {false, 0, fmt::format("cannot read '{}': {}", t_local_path, std::strerror(errno))};
    std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto res = client_.doUpload(t_remote_path, std::move(bytes), t_scope);
    if (!res.ok)
        return {false, res.http_status, res.message.empty() ? fmt::format("HTTP {}", res.http_status) : res.message};
    return {true, res.http_status, t_remote_path};
}

TransferOutcome StorageClient::download(const std::string& t_remote_path, const std::string& t_local_path, StorageScope t_scope) {
    auto res = client_.doDownload(t_remote_path, t_scope);
    if (!res.ok)
        return {false, res.http_status, res.message.empty() ? fmt::format("HTTP {}", res.http_status) : res.message};

    std::ofstream ofs(t_local_path, std::ios::binary);
    if (!ofs)
        return {false, 0, localWriteError(t_local_path)};
    ofs.write(res.bytes.data(), static_cast<std::streamsize>(res.bytes.size()));
    if (!ofs)
        return {false, 0, fmt::format("cannot write '{}': {}", t_local_path, std::strerror(errno))};
    return {true, res.http_status, t_local_path};
}

rapidjson::Document StorageClient::listFiles(const std::string& t_query_params) {
    return client_.query("files", t_query_params);
}

DriveListing StorageClient::listDrive(const std::string& t_path, StorageScope t_scope) {
    return client_.listDrive(t_path, t_scope);
}

sgrn::Result<DriveListing, std::string> StorageClient::tryListDrive(const std::string& t_path, StorageScope t_scope) {
    return client_.tryListDrive(t_path, t_scope);
}

bool StorageClient::createDirectory(const std::string& t_path, StorageScope t_scope) {
    return client_.createDriveDirectory(t_path, t_scope);
}

bool StorageClient::moveItem(
    int64_t t_id, DriveItemType t_type, const std::string& t_new_name, std::optional<int64_t> t_target_parent_id, StorageScope t_scope) {
    return client_.moveDriveItem(t_id, t_type, t_new_name, t_target_parent_id, t_scope);
}

bool StorageClient::deleteItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope) {
    return client_.deleteDriveItem(t_id, t_type, t_scope);
}

TransferOutcome StorageClient::downloadZip(const std::string& t_path, const std::string& t_local_path, StorageScope t_scope) {
    auto res = client_.doDownloadDriveZip(t_path, t_scope);
    if (!res.ok)
        return {false, res.http_status, res.message.empty() ? fmt::format("HTTP {}", res.http_status) : res.message};

    std::ofstream ofs(t_local_path, std::ios::binary);
    if (!ofs)
        return {false, 0, localWriteError(t_local_path)};
    ofs.write(res.bytes.data(), static_cast<std::streamsize>(res.bytes.size()));
    if (!ofs)
        return {false, 0, fmt::format("cannot write '{}': {}", t_local_path, std::strerror(errno))};
    return {true, res.http_status, t_local_path};
}

// ─── Telemetry Client ────────────────────────────────────────────────────────

TelemetryClient::TelemetryClient(DatastoreClient& t_client)
    : client_(t_client) {
}

void TelemetryClient::publish(const std::string& t_object_name, const rapidjson::Value& t_data) {
    client_.publishTelemetryAsync(t_object_name, t_data);
}

void TelemetryClient::publishJson(const rapidjson::Value& t_data) {
    client_.publishJsonTelemetryAsync(t_data);
}

void TelemetryClient::publishRaw([[maybe_unused]] const std::string& t_json) {
}

rapidjson::Document TelemetryClient::query(const std::string& t_query_params) {
    return client_.query("telemetry_data", t_query_params);
}

} // namespace sgrn::datastore::client
