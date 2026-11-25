// storage-uploads-internal.hpp — declarations shared by the storage-uploads*.cpp
// split files (storage-uploads.cpp = ingest, storage-uploads-finalize.cpp =
// complete/abort).
//
// The wire structs, SQL constants and session/actor helpers used to live in
// storage-uploads.cpp; both translation units need them now. The free
// functions are `inline` so every TU gets one definition. NOT part of the
// public handler interface — never include from outside
// sgrn/lib/datastore/src/handlers/.
#pragma once

#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/crypto/stream.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/file-signature.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/mime.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <json/value.h>
#include <map>
#include <openssl/evp.h>
#include <unordered_map>
#include <unordered_set>

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::ScopeContext;
using sgrn::datastore::services::storage::StorageScope;

namespace detail
{


struct UploadInitRequest {
    std::string filename;
    std::string target_path{"/"};
    std::string scope;
    std::string mime_type{"application/octet-stream"};
    int64_t total_size{0};
    int64_t chunk_size{0};
    bool encrypt{false};
};

struct UploadCompleteRequest {
    std::string upload_id;
};

struct UploadInitResponse {
    std::string upload_id;
    int64_t chunk_size{0};
    int32_t total_chunks{0};
    std::string status{"active"};
    bool is_encrypted{false};
};

struct UploadChunkResponse {
    std::string upload_id;
    int32_t chunk_index{0};
    bool received{true};
    std::size_t segments_flushed{0};
    bool duplicate{false};
};

struct UploadStatusResponse {
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

struct UploadStatusRow {
    std::string filename;
    std::string target_path;
    int64_t total_size{0};
    int64_t chunk_size{0};
    int32_t total_chunks{0};
    int32_t uploaded_chunks_count{0};
    std::string status;
};

struct UploadCompleteResponse {
    std::string upload_id;
    int64_t file_id{0};
    std::string file_hash;
    std::size_t segment_count{0};
    int64_t original_bytes{0};
    bool is_encrypted{false};
    std::string message{"Upload completed successfully"};
};

struct UploadAbortResponse {
    std::string message{"Upload session aborted"};
};

struct UploadUserQuotaRow {
    std::optional<int64_t> storage_limit;
    int64_t total_real_size{0};
    std::optional<int64_t> max_file_size_mb;
    std::optional<int32_t> preferred_chunk_size_mb;
};

struct DomainEncryptionPolicyRow {
    std::optional<std::string> encryption_policy;
};

struct UploadMimeStatusRow {
    std::string mime_type;
    std::string status;
};

struct UploadSessionCompletionRow {
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    int32_t total_chunks{0};
    int32_t uploaded_chunks_count{0};
    std::string filename;
    std::string target_path;
    int64_t total_size{0};
    std::optional<std::string> mime_type;
    bool is_encrypted{false};
    std::optional<std::string> domain;
    std::string status;
    bool expired{false};
    std::string scope;
    std::optional<int32_t> owner_user_id;
    std::optional<int32_t> owner_service_id;
    std::optional<int64_t> owner_session_id;
};

struct ChunkIndexRow {
    int32_t chunk_index{0};
};

struct ExistingSegmentRow {
    int64_t object_id{0};
    std::string key;
};

struct ObjectIdRow {
    int64_t id{0};
};

static constexpr char kUploadUserQuotaQuery[] =
    "SELECT storage_limit, total_real_size, max_file_size_mb, preferred_chunk_size_mb FROM core.users WHERE id = $1";
static constexpr char kDomainEncryptionPolicyQuery[] = "SELECT encryption_policy FROM core.domains WHERE name = $1 AND organisation = $2";
static constexpr char kUploadMimeStatusQuery[] =
    "SELECT mime_type, status FROM storage.upload_sessions WHERE id = $1 AND (user_id = $2 OR automated_service_id = $3)";
static constexpr char kUploadStatusQuery[] =
    "SELECT filename, target_path, total_size, chunk_size, total_chunks, uploaded_chunks_count, status "
    "FROM storage.upload_sessions WHERE id = $1 AND (user_id = $2 OR automated_service_id = $3)";
static constexpr char kUploadCompletionQuery[] =
    "SELECT user_id, automated_service_id, total_chunks, uploaded_chunks_count, filename, target_path, total_size, "
    "mime_type, is_encrypted, domain, status, (expires_at < NOW()) AS expired, scope, owner_user_id, owner_service_id, "
    "owner_session_id FROM storage.upload_sessions WHERE id = $1";

struct SessionActor {
    std::optional<int32_t> user_id;
    std::optional<int32_t> service_id;
};

inline SessionActor sessionActor(const ::sgrn::datastore::auth::SessionClaims& t_session) {
    SessionActor a;
    if (t_session.user.automated_service_id)
        a.service_id = t_session.user.automated_service_id;
    else if (t_session.user.id > 0)
        a.user_id = t_session.user.id;
    return a;
}

inline SessionActor sessionActor(const drogon::HttpRequestPtr& t_req) {
    if (t_req && t_req->attributes()->find("auth_context")) {
        const auto& auth = t_req->attributes()->get<filters::AuthContext>("auth_context");
        SessionActor a;
        if (auth.automated_service_id.has_value()) {
            a.service_id = auth.automated_service_id;
        } else if (auth.user.id > 0) {
            a.user_id = auth.user.id;
        }
        return a;
    }
    if (t_req && t_req->attributes()->find("session_claims")) {
        return sessionActor(t_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims"));
    }
    return {};
}

enum class UploadIdSource { Query, JsonBody, QueryOrJsonBody };

inline std::string uploadIdFromRequest(const drogon::HttpRequestPtr& t_req, UploadIdSource t_source = UploadIdSource::Query) {
    if (t_source != UploadIdSource::JsonBody) {
        std::string upload_id = t_req->getParameter("upload_id");
        if (!upload_id.empty() || t_source == UploadIdSource::Query) {
            return upload_id;
        }
    }

    if (t_source != UploadIdSource::Query) {
        auto req_res = parseTypedRequest<UploadCompleteRequest>(t_req);
        return req_res.hasError() ? std::string{} : req_res->upload_id;
    }
    return {};
}

inline bool hasUnsafePath(std::string_view t_path) {
    if (t_path.find('\0') != std::string_view::npos || t_path.find('\\') != std::string_view::npos)
        return true;
    size_t i = 0;
    while (i <= t_path.size()) {
        size_t j = t_path.find('/', i);
        if (j == std::string_view::npos)
            j = t_path.size();
        if (t_path.substr(i, j - i) == "..")
            return true;
        i = j + 1;
    }
    return false;
}

inline const char* scopeName(StorageScope t_scope) {
    switch (t_scope) {
        case StorageScope::Domain:
            return "domain";
        case StorageScope::Users:
            return "users";
        case StorageScope::AutomatedServices:
            return "automated_services";
        default:
            return "personal";
    }
}


} // namespace detail

// ── Segment size for the streaming upload pipeline ────────────────────────────
// Chunks received from the client are compressed and buffered; once the
// compressed buffer reaches kSegmentBytes, it is flushed as its own Garage
// object.  8 MiB is a good default: above S3's 5 MiB part minimum, small
// enough to keep memory bounded even for many concurrent uploads.
inline constexpr size_t kSegmentBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr uint8_t kCompressionLevel = 3;

// ── Flush one full segment from seg_buf to Garage ─────────────────────────
// Returns the new storage.objects id, or error.
inline Task<Result<int64_t, BackendError>> flushSegment(sgrn::datastore::plugins::aws::S3Client* s3, const drogon::orm::DbClientPtr& db,
    const std::string& bucket, const std::string& upload_id, int32_t seg_idx, std::vector<char>& seg_buf, const std::string& mime_type) {
    using namespace sgrn::datastore;
    using namespace sgrn::datastore::services::storage;

    // Build provisional key: pending/<upload_id>/<seg_idx>
    std::string seg_key = "pending/" + upload_id + "/" + std::to_string(seg_idx);
    std::string seg_data(seg_buf.begin(), seg_buf.end());
    const int64_t compressed_size = static_cast<int64_t>(seg_data.size());

    // Push segment to Garage
    auto put_res = co_await s3->uploadFromMemory(bucket, seg_key, seg_data, mime_type);
    CO_RETURN_ERROR_IF(put_res.hasError(), put_res.error());

    // original_size for a provisional segment is not known yet (we only know
    // total original_size at complete time). We store compressed_size as a
    // placeholder; it is corrected after the rename in handleCompleteUploadSession.
    // Compute SHA-512 of the compressed segment data as a placeholder.
    auto sha512_res = helpers::computeSha512InMemory(seg_data);
    CO_RETURN_ERROR_IF(sha512_res.hasError(), sha512_res.error());
    auto obj_res =
        co_await helpers::insertObject(db, bucket, seg_key, static_cast<size_t>(compressed_size), static_cast<size_t>(compressed_size),
            *sha512_res, /*is_compressed=*/true, std::string{"zstd"}, static_cast<std::optional<uint8_t>>(kCompressionLevel));
    co_return obj_res;
}

} // namespace sgrn::datastore::handlers::storage
