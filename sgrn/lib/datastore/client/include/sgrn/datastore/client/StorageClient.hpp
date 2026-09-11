#pragma once
#include <sgrn/Result.hpp>
#include <rapidjson/document.h>
#include <string>

#include <sgrn/datastore/client/Types.hpp>

namespace sgrn::datastore::client
{

class DatastoreClient;

/**
 * @brief Storage Client: Focuses on files, logs, and persistent objects.
 */
class StorageClient {
public:
    explicit StorageClient(DatastoreClient& t_client);

    /**
     * @brief Asynchronous file operations.
     */
    void uploadAsync(const std::string& t_remote_path, const std::string& t_local_path);
    void downloadAsync(const std::string& t_remote_path, const std::string& t_local_path);

    /**
     * @brief Synchronous file operations.
     */
    bool upload(const std::string& t_remote_path, const std::string& t_local_path, StorageScope t_scope = StorageScope::Auto);
    bool download(const std::string& t_remote_path, const std::string& t_local_path, StorageScope t_scope = StorageScope::Auto);

    rapidjson::Document listFiles(const std::string& t_query_params = "");

    // Drive API: typed browsing and file-system style operations.
    DriveListing listDrive(const std::string& t_path = "/", StorageScope t_scope = StorageScope::Auto);
    /// Error-reporting listDrive() variant
    sgrn::Result<DriveListing, std::string> tryListDrive(const std::string& t_path = "/", StorageScope t_scope = StorageScope::Auto);
    bool createDirectory(const std::string& t_path, StorageScope t_scope = StorageScope::Auto);
    bool moveItem(int64_t t_id, DriveItemType t_type, const std::string& t_new_path,
        std::optional<int64_t> t_target_parent_id = std::nullopt, StorageScope t_scope = StorageScope::Auto);
    bool deleteItem(int64_t t_id, DriveItemType t_type, StorageScope t_scope = StorageScope::Auto);
    bool downloadZip(const std::string& t_path, const std::string& t_local_path, StorageScope t_scope = StorageScope::Auto);

private:
    DatastoreClient& client_;
};

/**
 * @brief Telemetry Client: Focuses on telemetry and real-time metrics.
 */
class TelemetryClient {
public:
    explicit TelemetryClient(DatastoreClient& t_client);

    /**
     * @brief Asynchronous telemetry publishing.
     */
    void publish(const std::string& t_object_name, const rapidjson::Value& t_data);
    void publishJson(const rapidjson::Value& t_data);
    void publishRaw(const std::string& t_json);

    rapidjson::Document query(const std::string& t_query_params = "");

private:
    DatastoreClient& client_;
};

} // namespace sgrn::datastore::client
