#pragma once
// client-json.hpp — internal JSON mirror structs for the datastore client
// (Glaze). See sgrn/ml/src/spec-json.hpp for the pattern and naming precedent.
//
// Request bodies and response shapes are compatibility contracts with the
// backend API. Each shape is mirrored by a Json-suffixed struct whose member
// names match the JSON keys, so plain Glaze reflection reads and writes it;
// converters bridge to/from the public domain structs (whose members carry
// trailing underscores and can't match keys).
//
// Conditional keys (e.g. "secret" only when set) are std::optional members
// written with sgrn::json::writeOmitNull, which skips unset optionals —
// reproducing the historical "only emit non-defaults" rule.
//
// The id fields of some CRUD listings arrive as either numbers or strings;
// those endpoints keep tolerant field-by-field picking in Client.cpp instead
// of strict typed reads.

#include <sgrn/datastore/client/Client.hpp>
#include <sgrn/datastore/client/Types.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::client::detail
{

// ── Request bodies ───────────────────────────────────────────────────────────

struct SignInServiceJson {
    std::string token;
    std::string secret;
};

struct SignInUserJson {
    std::string email;
    std::string password;
};

struct NewUserJson {
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string password;
    std::string phone_number;
    std::string organisation;
    std::string status;
    std::string domain;
};

struct ServiceMetadataJson {
    std::optional<std::string> kind; ///< omitted when the service has no kind
};

struct ServiceRegistrationJson {
    std::string name;
    std::string organisation;
    std::string domain;
    ServiceMetadataJson metadata; ///< always emitted ({} without a kind)
};

struct RotateServiceTokenJson {
    int64_t automated_service_id{0};
};

struct PasswordUpdateJson {
    std::string old_password;
    std::string new_password;
};

struct PurgeOrphansJson {
    std::optional<std::string> bucket;
    std::optional<std::vector<std::string>> keys;
    std::optional<std::string> prefix;
    bool dry_run{false};
    uint32_t limit{0};
};

struct UploadInitJson {
    std::string filename;
    std::string target_path;
    std::string mime_type;
    int64_t total_size{0};
    int64_t chunk_size{0};
    bool encrypt{false};
};

struct UploadIdJson {
    std::string upload_id;
};

struct MoveDriveItemJson {
    std::optional<std::string> new_name;
    std::optional<int64_t> parent_id;
};

struct RegisterWebhookJson {
    std::string url;
    std::optional<std::string> secret; ///< omitted when empty
};

// ── Response bodies ──────────────────────────────────────────────────────────

/// doUpload 200/201 response: {"id": <int>, "key": "<object key>"}. Both
/// members are optional so an absent key leaves the UploadResult field unset
/// (the historical behavior).
struct UploadAckJson {
    std::optional<int64_t> id;
    std::optional<std::string> key;
};

struct GarageCensusJson {
    int64_t objects{0};
    int64_t bytes{0};
    bool truncated{false};
};

struct DbCensusJson {
    int64_t objects{0};
    int64_t bytes{0};
    int64_t files{0};
};

/// tryStorageAdminOverview "buckets" entries; the garage/db sub-objects are
/// optional so a missing census keeps the domain struct's zero defaults.
struct StorageBucketCensusJson {
    std::string name;
    std::optional<GarageCensusJson> garage;
    std::optional<DbCensusJson> db;
};

struct StorageOrphanKeyJson {
    std::string key;
    int64_t size{0};
    std::string etag;
};

struct StorageOrphansReportJson {
    std::string bucket;
    std::string prefix;
    int64_t garage_scanned{0};
    bool garage_truncated{false};
    std::vector<StorageOrphanKeyJson> garage_only;
    int64_t garage_only_count{0};
    int64_t garage_only_bytes{0};
    std::vector<StorageOrphanKeyJson> db_missing;
    int64_t db_missing_unchecked{0};
};

/// tryStorageAdminPurge: "would_delete" on dry runs, "deleted" otherwise.
struct StoragePurgeResultJson {
    bool dry_run{false};
    std::vector<std::string> would_delete;
    std::vector<std::string> deleted;
    std::vector<std::string> errors;
    std::vector<std::string> skipped;
};

struct ResumableUploadSessionJson {
    std::string upload_id;
    std::optional<int64_t> chunk_size; ///< absent keeps the 5 MiB domain default
    int32_t total_chunks{0};
    std::string status;
    bool is_encrypted{false};
};

struct ResumableUploadStatusJson {
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

// ── Drive listing mirrors ────────────────────────────────────────────────────
// "id" is an optional column: null/absent for synthetic path nodes.

struct DrivePathNodeJson {
    std::optional<int64_t> id;
    std::string name;
    std::string path;
    std::string display_name;
};

struct DriveFolderInfoJson {
    std::optional<int64_t> id;
    std::string name;
    std::string path;
    std::string display_name;
    std::string token;
    std::string kind;
    std::string created_at;
};

struct DriveFileInfoJson {
    int64_t id{0};
    std::string name;
    std::string path;
    std::string extension;
    int64_t size{0};
    int64_t original_size{0};
    std::string created_at;
    std::string sha256;
    std::string etag;
    bool is_encrypted{false};
    std::string key_id;
};

struct DriveListingJson {
    std::string path;
    std::vector<DrivePathNodeJson> trail;
    std::vector<DriveFolderInfoJson> folders;
    std::vector<DriveFileInfoJson> files;
};

// ── Converters ───────────────────────────────────────────────────────────────

[[nodiscard]] inline StorageBucketCensus censusFromJson(const StorageBucketCensusJson& t_w) {
    StorageBucketCensus c;
    c.name_ = t_w.name;
    if (t_w.garage) {
        c.garage_objects_ = t_w.garage->objects;
        c.garage_bytes_ = t_w.garage->bytes;
        c.garage_truncated_ = t_w.garage->truncated;
    }
    if (t_w.db) {
        c.db_objects_ = t_w.db->objects;
        c.db_bytes_ = t_w.db->bytes;
        c.db_files_ = t_w.db->files;
    }
    return c;
}

[[nodiscard]] inline StorageOrphansReport orphansReportFromJson(const StorageOrphansReportJson& t_w) {
    StorageOrphansReport r;
    r.bucket_ = t_w.bucket;
    r.prefix_ = t_w.prefix;
    r.garage_scanned_ = t_w.garage_scanned;
    r.garage_truncated_ = t_w.garage_truncated;
    r.garage_only_.reserve(t_w.garage_only.size());
    for (const auto& k : t_w.garage_only) {
        r.garage_only_.push_back({k.key, k.size, k.etag});
    }
    r.garage_only_count_ = t_w.garage_only_count;
    r.garage_only_bytes_ = t_w.garage_only_bytes;
    r.db_missing_.reserve(t_w.db_missing.size());
    for (const auto& k : t_w.db_missing) {
        r.db_missing_.push_back({k.key, k.size, k.etag});
    }
    r.db_missing_unchecked_ = t_w.db_missing_unchecked;
    return r;
}

[[nodiscard]] inline StoragePurgeResult purgeResultFromJson(const StoragePurgeResultJson& t_w) {
    StoragePurgeResult r;
    r.dry_run_ = t_w.dry_run;
    r.affected_ = t_w.dry_run ? t_w.would_delete : t_w.deleted;
    r.errors_ = t_w.errors;
    r.skipped_ = t_w.skipped;
    return r;
}

[[nodiscard]] inline DatastoreClient::ResumableUploadSession sessionFromJson(const ResumableUploadSessionJson& t_w) {
    DatastoreClient::ResumableUploadSession s;
    s.upload_id = t_w.upload_id;
    if (t_w.chunk_size) {
        s.chunk_size = *t_w.chunk_size;
    }
    s.total_chunks = t_w.total_chunks;
    s.status = t_w.status;
    s.is_encrypted = t_w.is_encrypted;
    return s;
}

[[nodiscard]] inline DatastoreClient::ResumableUploadStatus uploadStatusFromJson(const ResumableUploadStatusJson& t_w) {
    DatastoreClient::ResumableUploadStatus s;
    s.upload_id = t_w.upload_id;
    s.filename = t_w.filename;
    s.target_path = t_w.target_path;
    s.total_size = t_w.total_size;
    s.chunk_size = t_w.chunk_size;
    s.total_chunks = t_w.total_chunks;
    s.uploaded_chunks_count = t_w.uploaded_chunks_count;
    s.status = t_w.status;
    s.uploaded_chunk_indices = t_w.uploaded_chunk_indices;
    return s;
}

[[nodiscard]] inline DrivePathNode pathNodeFromJson(const DrivePathNodeJson& t_w) {
    DrivePathNode n;
    n.id_ = t_w.id;
    n.name_ = t_w.name;
    n.path = t_w.path;
    n.display_name = t_w.display_name;
    return n;
}

[[nodiscard]] inline DriveFolderInfo folderInfoFromJson(const DriveFolderInfoJson& t_w) {
    DriveFolderInfo f;
    f.id_ = t_w.id;
    f.name_ = t_w.name;
    f.path = t_w.path;
    f.display_name = t_w.display_name;
    f.token = t_w.token;
    f.kind = t_w.kind;
    f.created_at = t_w.created_at;
    return f;
}

[[nodiscard]] inline DriveFileInfo fileInfoFromJson(const DriveFileInfoJson& t_w) {
    DriveFileInfo f;
    f.id_ = t_w.id;
    f.name_ = t_w.name;
    f.path = t_w.path;
    f.extension = t_w.extension;
    f.size = t_w.size;
    f.original_size = t_w.original_size;
    f.created_at = t_w.created_at;
    f.sha256 = t_w.sha256;
    f.etag = t_w.etag;
    f.is_encrypted = t_w.is_encrypted;
    f.key_id = t_w.key_id;
    return f;
}

[[nodiscard]] inline DriveListing listingFromJson(const DriveListingJson& t_w) {
    DriveListing l;
    l.path = t_w.path;
    l.trail.reserve(t_w.trail.size());
    for (const auto& n : t_w.trail) {
        l.trail.push_back(pathNodeFromJson(n));
    }
    l.folders.reserve(t_w.folders.size());
    for (const auto& f : t_w.folders) {
        l.folders.push_back(folderInfoFromJson(f));
    }
    l.files.reserve(t_w.files.size());
    for (const auto& f : t_w.files) {
        l.files.push_back(fileInfoFromJson(f));
    }
    return l;
}

} // namespace sgrn::datastore::client::detail
