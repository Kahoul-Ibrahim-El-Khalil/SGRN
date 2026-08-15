#include <sgrn/sdk/Domains.hpp>
#include <sgrn/sdk/SgrnClient.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <fstream>

namespace sgrn::sdk
{

// ─── Storage Client ──────────────────────────────────────────────────────────

StorageClient::StorageClient(SgrnClient& t_client)
    : client_(t_client) {
}

void StorageClient::uploadAsync(const std::string& t_remote_path, const std::string& t_local_path) {
    client_.uploadFileAsync(t_remote_path, t_local_path);
}

void StorageClient::downloadAsync([[maybe_unused]] const std::string& t_remote_path, [[maybe_unused]] const std::string& t_local_path) {
    // Currently downloads are handled synchronously for ease of use,
    // but could be queued if needed. For now, we delegate to the async queue if requested.
    // client_.enqueueTask(TaskType::Download, ...);
}

bool StorageClient::upload(const std::string& t_remote_path, const std::string& t_local_path) {
    std::ifstream ifs(t_local_path, std::ios::binary);
    if (!ifs)
        return false;
    std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return client_.doUpload(t_remote_path, std::move(bytes)).ok;
}

bool StorageClient::download(const std::string& t_remote_path, const std::string& t_local_path) {
    auto res = client_.doDownload(t_remote_path);
    if (!res.ok)
        return false;

    std::ofstream ofs(t_local_path, std::ios::binary);
    if (!ofs)
        return false;
    ofs.write(res.bytes.data(), static_cast<std::streamsize>(res.bytes.size()));
    return true;
}

rapidjson::Document StorageClient::listFiles(const std::string& t_query_params) {
    return client_.query("files", t_query_params);
}

DriveListing StorageClient::listDrive(const std::string& t_path, StorageScope t_scope) {
    return client_.listDrive(t_path, t_scope);
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

bool StorageClient::downloadZip(const std::string& t_path, const std::string& t_local_path, StorageScope t_scope) {
    auto res = client_.doDownloadDriveZip(t_path, t_scope);
    if (!res.ok)
        return false;

    std::ofstream ofs(t_local_path, std::ios::binary);
    if (!ofs)
        return false;
    ofs.write(res.bytes.data(), static_cast<std::streamsize>(res.bytes.size()));
    return true;
}

// ─── Telemetry Client ────────────────────────────────────────────────────────

TelemetryClient::TelemetryClient(SgrnClient& t_client)
    : client_(t_client) {
}

void TelemetryClient::publish(const std::string& t_object_name, const rapidjson::Value& t_data) {
    client_.publishTelemetryAsync(t_object_name, t_data);
}

void TelemetryClient::publishJson(const rapidjson::Value& t_data) {
    client_.publishJsonTelemetryAsync(t_data);
}

void TelemetryClient::publishRaw([[maybe_unused]] const std::string& t_json) {
    // client_.enqueueRawTelemetry(t_json); // Need to implement this if needed, or just publishJson
}

rapidjson::Document TelemetryClient::query(const std::string& t_query_params) {
    return client_.query("telemetry_data", t_query_params);
}

} // namespace sgrn::sdk
