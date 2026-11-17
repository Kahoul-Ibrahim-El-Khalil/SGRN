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
#include <sgrn/datastore/wire/wire.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::client::detail
{

// ── Request bodies ───────────────────────────────────────────────────────────

// Requests that already have a shared wire type: use it directly.
using NewUserJson = sgrn::datastore::admin::NewUser;
using PasswordUpdateJson = sgrn::datastore::auth::UpdatePasswordRequest;

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

using RegisterWebhookJson = sgrn::datastore::admin::RegisterWebhookRequest;

// ── Response bodies ──────────────────────────────────────────────────────────

/// doUpload 200/201 response: {"id": <int>, "key": "<object key>"}. Both
/// members are optional so an absent key leaves the UploadResult field unset
/// (the historical behavior).
struct UploadAckJson {
    std::optional<int64_t> id;
    std::optional<std::string> key;
};

// Purge has two response shapes that the client normalises into one result.
using PurgeDryRunJson = sgrn::datastore::storage::PurgeDryRunResponse;
using PurgeResponseJson = sgrn::datastore::storage::PurgeResponse;

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

// ── Converters (defined in client-json.cpp) ──────────────────────────────────
//
// Only shapes that differ from the public type need a converter. Drive
// listings, storage censuses and orphan reports are decoded directly into the
// public wire types.

[[nodiscard]] StoragePurgeResult purgeResultFromJson(const PurgeDryRunJson& t_w);

[[nodiscard]] StoragePurgeResult purgeResultFromJson(const PurgeResponseJson& t_w);

[[nodiscard]] DatastoreClient::ResumableUploadSession sessionFromJson(const ResumableUploadSessionJson& t_w);

[[nodiscard]] DatastoreClient::ResumableUploadStatus uploadStatusFromJson(const ResumableUploadStatusJson& t_w);

} // namespace sgrn::datastore::client::detail
