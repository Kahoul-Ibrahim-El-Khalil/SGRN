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

#ifdef DEBUG_STORAGE_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif


#include "debug-http-request.hpp"
#include "debug-pretty-json.hpp"
#include "storage-shared.hpp"
#include "storage-uploads-internal.hpp"

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::ScopeContext;
using sgrn::datastore::services::storage::StorageScope;

using namespace detail;

Task<HttpResponsePtr> StorageApiHandler::handleInitUploadSession(HttpRequestPtr tsp_req) {
    DEBUG_HTTP_REQUEST(tsp_req, "handleInitUploadSession");
    auto req_res = parseTypedRequest<UploadInitRequest>(tsp_req);
    if (req_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{"Invalid JSON payload", k400BadRequest});
    }
    const auto& req = req_res.value();

    const std::string& filename = req.filename;
    const std::string& raw_target = req.target_path.empty() ? "/" : req.target_path;
    std::string scope_str = req.scope;
    const std::string& mime_type = req.mime_type.empty() ? "application/octet-stream" : req.mime_type;
    const int64_t total_size = req.total_size;
    int64_t chunk_size = 0;
    if (req.chunk_size > 0) {
        chunk_size = req.chunk_size;
    } else if (total_size > static_cast<int64_t>(5ULL * 1024ULL * 1024ULL * 1024ULL)) {
        chunk_size = 32 * 1024 * 1024;
    } else if (total_size > 500 * 1024 * 1024) {
        chunk_size = 16 * 1024 * 1024;
    } else if (total_size > 50 * 1024 * 1024) {
        chunk_size = 8 * 1024 * 1024;
    } else {
        chunk_size = 5 * 1024 * 1024;
    }

    if (filename.empty() || total_size <= 0 || chunk_size <= 0) {
        co_return createTypedResponse(BackendErrorResponse{"filename, total_size, and chunk_size are required", k400BadRequest});
    }
    if (filename.find_first_of("/\\") != std::string::npos || filename.find('\0') != std::string::npos || filename == "." ||
        filename == "..") {
        co_return createTypedResponse(BackendErrorResponse{"Invalid filename", k400BadRequest});
    }
    if (hasUnsafePath(raw_target)) {
        co_return createTypedResponse(BackendErrorResponse{"Invalid target_path", k400BadRequest});
    }
    auto norm = normalizePath(raw_target.empty() ? std::string("/") : raw_target);
    if (!norm) {
        co_return createTypedResponse(BackendErrorResponse{"Invalid target_path", k400BadRequest});
    }
    const std::string requested_path = *norm;

    // Older clients (CLI rput) send no scope: infer it from the canonical prefix.
    if (scope_str.empty()) {
        scope_str = (requested_path.rfind("/domains/", 0) == 0) ? "domain" : "personal";
    }

    const SessionActor actor = sessionActor(tsp_req);
    if (!actor.user_id && !actor.service_id) {
        co_return createTypedResponse(BackendErrorResponse{"Unauthenticated", k401Unauthorized});
    }
    const auto& session = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        // ── Resolve the real destination (same code path as the non-chunked upload) ──
        const StorageScope scope = storage_service_.parseScope(scope_str);
        auto scope_res = co_await storage_service_.resolveScopeSession(session, scope, requested_path);
        CO_RETURN_IF(scope_res.hasError(), createTypedResponse(BackendErrorResponse{scope_res.error()}));
        const ScopeContext ctx = scope_res.value();

        if (ctx.is_virtual_root) {
            co_return createTypedResponse(
                BackendErrorResponse{"Select a folder inside a domain, user or service namespace", k400BadRequest});
        }
        if (!ctx.can_write) {
            co_return createTypedResponse(BackendErrorResponse{"You do not have write permission for this location", k403Forbidden});
        }
        if (scope == StorageScope::Domain && ctx.domain.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Could not resolve target domain", k400BadRequest});
        }

        // ── User constraints (quota, max file size, chunk size) ──
        if (actor.user_id) {
            auto user_row = co_await db->execSqlCoro(kUploadUserQuotaQuery, *actor.user_id);
            if (!user_row.empty()) {
                const auto u = sgrn::orm::fromRow<UploadUserQuotaRow, kUploadUserQuotaQuery>(user_row[0]);
                if (u.max_file_size_mb) {
                    const int64_t max_bytes = *u.max_file_size_mb * 1024 * 1024;
                    if (total_size > max_bytes) {
                        co_return createTypedResponse(BackendErrorResponse{
                            fmt::format("File size ({:.1f} MB) exceeds your account maximum allowed file size ({:.1f} MB)",
                                static_cast<double>(total_size) / (1024.0 * 1024.0), static_cast<double>(max_bytes) / (1024.0 * 1024.0)),
                            k400BadRequest});
                    }
                }
                if (u.storage_limit) {
                    if (u.total_real_size + total_size > *u.storage_limit) {
                        co_return createTypedResponse(BackendErrorResponse{
                            "Storage quota exceeded. Please delete unused files or request a quota increase.", k413RequestEntityTooLarge});
                    }
                }
                if (u.preferred_chunk_size_mb && *u.preferred_chunk_size_mb > 0) {
                    chunk_size = static_cast<int64_t>(*u.preferred_chunk_size_mb) * 1024 * 1024;
                }
            }
        }
        const int32_t total_chunks = static_cast<int32_t>((total_size + chunk_size - 1) / chunk_size);

        // ── Encryption policy: only domain targets are governed, and it fails CLOSED ──
        bool encrypt = req.encrypt;
        if (!ctx.domain.empty()) {
            const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);
            if (org.empty()) {
                co_return createTypedResponse(BackendErrorResponse{"Organisation claim missing from session", k403Forbidden});
            }
            auto dom_rows = co_await db->execSqlCoro(kDomainEncryptionPolicyQuery, ctx.domain, org);
            if (dom_rows.empty()) {
                co_return createTypedResponse(BackendErrorResponse{"Unknown domain '" + ctx.domain + "'", k404NotFound});
            }
            const auto domain = sgrn::orm::fromRow<DomainEncryptionPolicyRow, kDomainEncryptionPolicyQuery>(dom_rows[0]);
            if (!domain.encryption_policy) {
                co_return createTypedResponse(BackendErrorResponse{"Unknown domain '" + ctx.domain + "'", k404NotFound});
            }
            const std::string& policy = *domain.encryption_policy;
            if (policy == "required") {
                encrypt = true; // upgrade silently; the response tells the client
            } else if (policy == "none") {
                encrypt = false; // downgrade silently
            } else if (policy != "optional") {
                co_return createTypedResponse(BackendErrorResponse{"Domain has an invalid encryption policy", k500InternalServerError});
            }
        }

        const std::string upload_id = drogon::utils::getUuid();
        std::optional<std::string> domain_opt = ctx.domain.empty() ? std::nullopt : std::optional<std::string>(ctx.domain);
        std::optional<int64_t> owner_session = ctx.t_session_id > 0 ? std::optional<int64_t>(ctx.t_session_id) : std::nullopt;

        co_await db->execSqlCoro(
            "INSERT INTO storage.upload_sessions (id, user_id, automated_service_id, target_path, filename, mime_type, total_size, "
            "chunk_size, total_chunks, is_encrypted, domain, scope, owner_user_id, owner_service_id, owner_session_id) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)",
            upload_id, actor.user_id, actor.service_id, ctx.actual_path, filename, mime_type, total_size, chunk_size, total_chunks, encrypt,
            domain_opt, std::string(scopeName(scope)), ctx.user_id, ctx.t_automated_service_id, owner_session);

        co_return createTypedResponse(tsp_req,
            UploadInitResponse{
                .upload_id = upload_id,
                .chunk_size = chunk_size,
                .total_chunks = total_chunks,
                .status = "active",
                .is_encrypted = encrypt,
            },
            k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Init upload session error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{"Failed to initialize upload session", k500InternalServerError});
    }
}

// ── UploadSessionState helpers ─────────────────────────────────────────────
std::shared_ptr<StorageApiHandler::UploadSessionState> StorageApiHandler::getOrCreateUploadState(
    const std::string& t_upload_id, const std::string& t_mime_type, uint8_t t_level) {
    std::lock_guard<std::mutex> lock(upload_states_mutex_);
    auto it = upload_states_.find(t_upload_id);
    if (it != upload_states_.end())
        return it->second;

    auto state = std::make_shared<UploadSessionState>();

    // SHA-512 context
    state->hash_ctx = EVP_MD_CTX_new();
    if (!state->hash_ctx)
        return nullptr;
    if (EVP_DigestInit_ex(state->hash_ctx, EVP_sha512(), nullptr) != 1)
        return nullptr;

    // Only compress if the MIME type is compressible (e.g. text/json/xml).
    // For already-compressed or binary formats, store verbatim.
    if (sgrn::utils::mime::isCompressibleMimeType(t_mime_type)) {
        state->cstream = ZSTD_createCStream();
        if (!state->cstream)
            return nullptr;
        ZSTD_initCStream(state->cstream, t_level);
        state->server_compressed = true;
    }

    state->mime_type = t_mime_type;
    upload_states_[t_upload_id] = state;
    return state;
}

void StorageApiHandler::removeUploadState(const std::string& t_upload_id) {
    std::lock_guard<std::mutex> lock(upload_states_mutex_);
    upload_states_.erase(t_upload_id);
}

Task<HttpResponsePtr> StorageApiHandler::handleUploadChunk(HttpRequestPtr tsp_req) {
    DEBUG_HTTP_REQUEST(tsp_req, "handleUploadChunk");
    std::string upload_id = uploadIdFromRequest(tsp_req);
    std::string chunk_idx_str = tsp_req->getParameter("chunk_index");
    if (upload_id.empty() || chunk_idx_str.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"upload_id and chunk_index parameters are required", k400BadRequest});
    }

    int32_t chunk_index = 0;
    try {
        chunk_index = std::stoi(chunk_idx_str);
    } catch (...) {
        co_return createTypedResponse(BackendErrorResponse{"chunk_index must be an integer", k400BadRequest});
    }

    const std::string_view body = tsp_req->getBody();
    if (body.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"Chunk payload is empty", k400BadRequest});
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        // ── 1. Load session to get mime_type ────────────────────────────────
        const SessionActor actor = sessionActor(tsp_req);
        auto s_rows = co_await db->execSqlCoro(kUploadMimeStatusQuery, upload_id, actor.user_id, actor.service_id);
        if (s_rows.empty())
            co_return createTypedResponse(BackendErrorResponse{"Upload session not found", k404NotFound});
        const auto upload = sgrn::orm::fromRow<UploadMimeStatusRow, kUploadMimeStatusQuery>(s_rows[0]);
        if (upload.status != "active")
            co_return createTypedResponse(BackendErrorResponse{"Upload session is not active", k400BadRequest});
        const std::string& mime_type = upload.mime_type;

        // ── 2. Get-or-create the per-session streaming state ────────────────
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);
        if (!state) {
            co_return createTypedResponse(BackendErrorResponse{"Failed to allocate upload stream state", k500InternalServerError});
        }

        // ── 2b. Streaming state is strictly sequential (hash + zstd + seg_buf) ──
        if (chunk_index < state->processed_chunks) {
            // client retry after a lost response: already consumed, do NOT feed it twice
            co_return createTypedResponse(tsp_req,
                UploadChunkResponse{
                    .upload_id = upload_id,
                    .chunk_index = chunk_index,
                    .received = true,
                    .segments_flushed = 0,
                    .duplicate = true,
                },
                k200OK);
        }
        if (chunk_index != state->processed_chunks) {
            co_return createTypedResponse(BackendErrorResponse{
                fmt::format("Out-of-order chunk: expected {} (stream state is in-memory; restart the upload if the server restarted)",
                    state->processed_chunks),
                k409Conflict});
        }

        auto s3_res = storage_service_.S3Client();
        CO_RETURN_IF(s3_res.hasError(),
            createTypedResponse(BackendErrorResponse{fmt::format("S3 unavailable: {}", s3_res.error().message), k500InternalServerError}));
        auto* s3 = s3_res.value();
        const services::storage::StorageConfig live_cfg = services::storage::currentStorageConfig();
        const std::string bucket = live_cfg.default_bucket;

        // ── 3. Feed raw bytes through hash + compressor ─────────────────────
        // (No mutex needed here — Drogon serialises requests per upload_id
        //  because the client sends chunks sequentially per session.)
        EVP_DigestUpdate(state->hash_ctx, body.data(), body.size());
        state->original_bytes += static_cast<int64_t>(body.size());

        if (state->server_compressed) {
            // Feed body into zstd streaming compressor
            ZSTD_inBuffer zstd_in{body.data(), body.size(), 0};
            const size_t out_buf_size = ZSTD_CStreamOutSize();
            std::vector<char> out_buf(out_buf_size);

            while (zstd_in.pos < zstd_in.size) {
                ZSTD_outBuffer zstd_out{out_buf.data(), out_buf_size, 0};
                size_t ret = ZSTD_compressStream(state->cstream, &zstd_out, &zstd_in);
                if (ZSTD_isError(ret)) {
                    co_return createTypedResponse(
                        BackendErrorResponse{fmt::format("Compression error: {}", ZSTD_getErrorName(ret)), k500InternalServerError});
                }
                state->seg_buf.insert(state->seg_buf.end(), out_buf.data(), out_buf.data() + zstd_out.pos);
            }
        } else {
            // Non-compressible MIME: buffer raw bytes directly
            state->seg_buf.insert(state->seg_buf.end(), body.data(), body.data() + body.size());
        }

        // ── 4. Flush complete segments ───────────────────────────────────────
        // Record object_ids for each segment pushed this chunk.
        std::vector<std::pair<int32_t, int64_t>> new_segments; // {seg_idx, object_id}

        while (state->seg_buf.size() >= kSegmentBytes) {
            // Cut exactly kSegmentBytes from the front of seg_buf.
            std::vector<char> seg_data(state->seg_buf.begin(), state->seg_buf.begin() + kSegmentBytes);
            state->seg_buf.erase(state->seg_buf.begin(), state->seg_buf.begin() + kSegmentBytes);

            int32_t seg_idx = state->next_seg_idx++;
            // Temporarily swap into the flushSegment-friendly overwrite
            auto flush_res = co_await flushSegment(s3, db, bucket, upload_id, seg_idx, seg_data, mime_type);
            if (flush_res.hasError()) {
                co_return createTypedResponse(BackendErrorResponse{
                    fmt::format("Failed to store segment {}: {}", seg_idx, flush_res.error().message), k500InternalServerError});
            }
            new_segments.emplace_back(seg_idx, flush_res.value());
        }

        // ── 5. Persist chunk record and update session ───────────────────────
        if (!new_segments.empty()) {
            for (const auto& [seg_idx, seg_obj_id] : new_segments) {
                co_await db->execSqlCoro(
                    "INSERT INTO storage.upload_chunks (upload_id, chunk_index, chunk_size, storage_key, object_id, compressed_size) "
                    "VALUES ($1, $2, $3, $4, $5, $6) ON CONFLICT (upload_id, chunk_index) DO UPDATE "
                    "SET chunk_size = EXCLUDED.chunk_size, object_id = EXCLUDED.object_id, "
                    "    compressed_size = EXCLUDED.compressed_size",
                    upload_id, seg_idx, static_cast<int64_t>(body.size()), "pending/" + upload_id + "/" + std::to_string(seg_idx),
                    seg_obj_id, static_cast<int64_t>(kSegmentBytes));
            }
        } else {
            // No segment flushed yet (compressed bytes still buffering)
            co_await db->execSqlCoro("INSERT INTO storage.upload_chunks (upload_id, chunk_index, chunk_size, storage_key) "
                                     "VALUES ($1, $2, $3, $4) ON CONFLICT (upload_id, chunk_index) DO UPDATE "
                                     "SET chunk_size = EXCLUDED.chunk_size",
                upload_id, chunk_index, static_cast<int64_t>(body.size()), "pending/" + upload_id + "/buffering");
        }

        state->processed_chunks++;

        co_await db->execSqlCoro("UPDATE storage.upload_sessions "
                                 "SET uploaded_chunks_count = $2, "
                                 "    updated_at = NOW() WHERE id = $1",
            upload_id, state->processed_chunks);

        co_return createTypedResponse(tsp_req,
            UploadChunkResponse{
                .upload_id = upload_id,
                .chunk_index = chunk_index,
                .received = true,
                .segments_flushed = new_segments.size(),
                .duplicate = false,
            },
            k200OK);

    } catch (const std::exception& e) {
        ERROR_LOG("Upload chunk error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{"Failed storing chunk", k500InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleGetUploadStatus(HttpRequestPtr tsp_req) {
    DEBUG_HTTP_REQUEST(tsp_req, "handleGetUploadStatus");
    std::string upload_id = uploadIdFromRequest(tsp_req);

    CO_RETURN_IF(upload_id.empty(), createTypedResponse(BackendErrorResponse{"upload_id is required", k400BadRequest}));

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        const SessionActor actor = sessionActor(tsp_req);
        auto s_rows = co_await db->execSqlCoro(kUploadStatusQuery, upload_id, actor.user_id, actor.service_id);

        CO_RETURN_IF(s_rows.empty(), createTypedResponse(BackendErrorResponse{"Upload session not found", k404NotFound}));

        static constexpr char kChunkIndexQuery[] =
            "SELECT chunk_index FROM storage.upload_chunks WHERE upload_id = $1 ORDER BY chunk_index ASC";

        auto idx_rows = co_await db->execSqlCoro(kChunkIndexQuery, upload_id);

        std::vector<int32_t> uploaded_chunk_indices;

        uploaded_chunk_indices.reserve(idx_rows.size());

        for (const auto& idx_row : sgrn::orm::fromResult<ChunkIndexRow, kChunkIndexQuery>(idx_rows)) {
            uploaded_chunk_indices.push_back(idx_row.chunk_index);
        }

        const auto row = sgrn::orm::fromRow<UploadStatusRow, kUploadStatusQuery>(s_rows[0]);
        UploadStatusResponse response{
            .upload_id = upload_id,
            .filename = row.filename,
            .target_path = row.target_path,
            .total_size = row.total_size,
            .chunk_size = row.chunk_size,
            .total_chunks = row.total_chunks,
            .uploaded_chunks_count = row.uploaded_chunks_count,
            .status = row.status,
            .uploaded_chunk_indices = std::move(uploaded_chunk_indices),
        };
        co_return createTypedResponse(tsp_req, response, k200OK);

    } catch (const std::exception& e) {
        ERROR_LOG("Get upload status error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{"Failed fetching upload status", k500InternalServerError});
    }
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
