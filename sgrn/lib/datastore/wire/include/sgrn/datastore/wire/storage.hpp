#pragma once
// storage.hpp — storage / drive wire types shared by server and clients.

#include <sgrn/utils/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::storage
{

struct DrivePathNode {
    std::optional<int64_t> id{};
    std::string name{};
    std::string path{};
    std::string display_name{};
};

struct DriveFolderInfo {
    std::optional<int64_t> id{};
    std::string name{};
    std::string path{};
    std::string display_name{};
    std::string token{};
    std::string kind{};
    std::string created_at{};
};

struct DriveFileInfo {
    int64_t id{};
    std::string name{};
    std::string path{};
    std::string extension{};
    int64_t size{};
    int64_t original_size{};
    std::string created_at{};
    std::string sha256{};
    std::string etag{};
    bool is_encrypted{false};
    std::string key_id{};
};

struct DriveListing {
    std::string path{};
    std::vector<DrivePathNode> trail{};
    std::vector<DriveFolderInfo> folders{};
    std::vector<DriveFileInfo> files{};
};

/// GET /api/v1/storage/stats
struct StorageUsage {
    int64_t file_count{0};
    int64_t total_original_bytes{0};
    int64_t total_compressed_bytes{0};
    std::optional<int64_t> storage_limit;
};

/// GET /api/v1/storage/info
struct StorageConstraints {
    uint64_t max_file_size_mb{0};
    uint64_t max_file_size_bytes{0};
    bool encryption_supported{false};
};

// ─── Storage administration reports (shapes emitted by StorageAdminHandler) ─

struct GarageCensus {
    int64_t objects{0};
    int64_t bytes{0};
    bool truncated{false};
    uint32_t pages{0};
};

struct DbCensus {
    int64_t objects{0};
    int64_t bytes{0};
    int64_t files{0};
};

struct StorageBucketCensus {
    std::string name;
    GarageCensus garage;
    DbCensus db;
};

struct StorageOverview {
    std::vector<StorageBucketCensus> buckets;
};

struct StorageOrphanKey {
    std::string key;
    int64_t size{0};
    std::string etag;
};

/// db_missing entries carry no etag on the server side.
struct StorageOrphanSize {
    std::string key;
    int64_t size{0};
};

struct StorageOrphansReport {
    std::string bucket;
    std::string prefix;
    int64_t garage_scanned{0};
    bool garage_truncated{false};
    std::vector<StorageOrphanKey> garage_only;
    int64_t garage_only_count{0};
    int64_t garage_only_bytes{0};
    std::vector<StorageOrphanSize> db_missing;
    int64_t db_missing_unchecked{0};
};

struct PurgeSkipped {
    std::string key;
    std::string reason;
};

struct PurgeKey {
    std::string key;
};

/// Dry-run purge: "would_delete" is an array of {key} objects, not strings.
struct PurgeDryRunResponse {
    std::string bucket;
    bool dry_run{true};
    std::vector<PurgeKey> would_delete;
    int64_t would_delete_count{0};
    std::vector<PurgeSkipped> skipped;
};

/// Real purge: "errors" is whatever the S3 upstream reported (array of
/// objects), so it stays a dynamic value rather than vector<string>.
struct PurgeResponse {
    std::string bucket;
    bool dry_run{false};
    std::vector<std::string> deleted;
    std::optional<sgrn::json::Value> errors;
    std::vector<PurgeSkipped> skipped;
};

struct ObjectInfo {
    int32_t id{};
    std::string name{};
    std::string status{};
    std::optional<sgrn::json::Value> metadata{};
};

} // namespace sgrn::datastore::storage
