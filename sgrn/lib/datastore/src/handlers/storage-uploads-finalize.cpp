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

static Task<void> rollbackEncryptedUpload(sgrn::datastore::plugins::aws::S3Client* s3, const drogon::orm::DbClientPtr& db,
    const std::string& bucket, const std::string& obj_key, const std::string& key_id, int64_t object_id, std::optional<int64_t> file_id) {
    if (auto ks = sgrn::datastore::crypto::getKeyStore())
        (void)ks->shred(key_id);
    if (file_id)
        co_await db->execSqlCoro("DELETE FROM storage.files WHERE id = $1", *file_id);
    co_await s3->deleteFile(bucket, obj_key);
    co_await db->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", object_id);
}

Task<HttpResponsePtr> StorageApiHandler::handleCompleteUploadSession(HttpRequestPtr tsp_req) {
    DEBUG_HTTP_REQUEST(tsp_req, "handleCompleteUploadSession");
    const std::string upload_id = uploadIdFromRequest(tsp_req, UploadIdSource::JsonBody);

    CO_RETURN_IF(upload_id.empty(), createTypedResponse(BackendErrorResponse{"upload_id is required", k400BadRequest}));

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));
        auto db = db_res.value();

        // -- 1. Load and validate session
        auto s_rows = co_await db->execSqlCoro(kUploadCompletionQuery, upload_id);
        if (s_rows.empty())
            co_return createTypedResponse(BackendErrorResponse{"Upload session not found", k404NotFound});

        const auto s = sgrn::orm::fromRow<UploadSessionCompletionRow, kUploadCompletionQuery>(s_rows[0]);
        const SessionActor actor = sessionActor(tsp_req);
        const bool is_uploader = (s.user_id && actor.user_id && *s.user_id == *actor.user_id) ||
                                 (s.automated_service_id && actor.service_id && *s.automated_service_id == *actor.service_id);

        CO_RETURN_IF(!is_uploader, createTypedResponse(BackendErrorResponse{"Upload session not found", k404NotFound}));

        CO_RETURN_IF(s.status != "active", createTypedResponse(BackendErrorResponse{"Upload session is not active", k409Conflict}));

        CO_RETURN_IF(s.expired, createTypedResponse(BackendErrorResponse{"Upload session expired", k410Gone}));

        const int32_t total_chunks = s.total_chunks;

        CO_RETURN_IF(s.uploaded_chunks_count < total_chunks,
            createTypedResponse(BackendErrorResponse{"Cannot complete: not all chunks have been uploaded", k400BadRequest}));

        const std::string& filename = s.filename;
        const std::string& target_path = s.target_path;
        const int64_t declared_size = s.total_size;
        const std::string mime_type = s.mime_type.value_or("application/octet-stream");
        const bool is_encrypted = s.is_encrypted;
        const std::string domain_str = s.domain.value_or("");
        const std::string& scope_name = s.scope;

        // Owner of the resulting file (resolved at init, never re-derived from the path)
        std::optional<int32_t> owner_user = s.owner_user_id;
        std::optional<int32_t> owner_service = s.owner_service_id;
        if (!owner_user && !owner_service) { // fallback: uploader
            owner_user = s.user_id;
            owner_service = s.automated_service_id;
        }

        int64_t owner_session = 0;
        if (tsp_req->attributes()->find("auth_context")) {
            owner_session = tsp_req->attributes()->get<filters::AuthContext>("auth_context").session_id;
        } else if (tsp_req->attributes()->find("session_claims")) {
            owner_session = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").session_id;
        }
        if ((scope_name == "users" || scope_name == "automated_services") && s.owner_session_id && *s.owner_session_id > 0) {
            owner_session = *s.owner_session_id;
        }

        // -- 2. Recover streaming pipeline state
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);

        CO_RETURN_IF(!state, createTypedResponse(BackendErrorResponse{"Failed to recover upload stream state", k500InternalServerError}));

        auto s3_res = storage_service_.S3Client();

        CO_RETURN_IF(s3_res.hasError(),
            createTypedResponse(BackendErrorResponse{fmt::format("S3 unavailable: {}", s3_res.error().message), k500InternalServerError}));
        auto* s3 = s3_res.value();
        const services::storage::StorageConfig live_cfg = services::storage::currentStorageConfig();
        const std::string bucket = live_cfg.default_bucket;

        // -- 3. Collect already-flushed segments from DB
        static constexpr char kSegmentQuery[] = "SELECT uc.object_id, o.key "
                                                "FROM storage.upload_chunks uc "
                                                "JOIN storage.objects o ON o.id = uc.object_id "
                                                "WHERE uc.upload_id = $1 AND uc.object_id IS NOT NULL";
        auto seg_rows = co_await db->execSqlCoro(kSegmentQuery, upload_id);

        std::map<int32_t, int64_t> existing_segs;
        for (const auto& row : sgrn::orm::fromResult<ExistingSegmentRow, kSegmentQuery>(seg_rows)) {
            const std::string& key = row.key;
            const int64_t oid = row.object_id;
            auto slash = key.rfind('/');
            if (slash != std::string::npos) {
                try {
                    existing_segs.emplace(std::stoi(key.substr(slash + 1)), oid);
                } catch (...) {
                }
            }
        }

        // -- 4. Flush remaining compressed bytes (zstd endStream)
        if (state->server_compressed && state->cstream) {
            const size_t out_buf_size = ZSTD_CStreamOutSize();
            std::vector<char> out_buf(out_buf_size);
            bool done = false;
            while (!done) {
                ZSTD_outBuffer zstd_out{out_buf.data(), out_buf_size, 0};
                size_t ret = ZSTD_endStream(state->cstream, &zstd_out);
                if (ZSTD_isError(ret)) {
                    removeUploadState(upload_id);
                    co_return createTypedResponse(
                        BackendErrorResponse{fmt::format("Compression flush error: {}", ZSTD_getErrorName(ret)), k500InternalServerError});
                }
                state->seg_buf.insert(state->seg_buf.end(), out_buf.data(), out_buf.data() + zstd_out.pos);
                done = (ret == 0);
            }
        }

        if (!state->seg_buf.empty()) {
            int32_t seg_idx = state->next_seg_idx++;
            auto flush_res = co_await flushSegment(s3, db, bucket, upload_id, seg_idx, state->seg_buf, mime_type);
            if (flush_res.hasError()) {
                removeUploadState(upload_id);
                co_return createTypedResponse(BackendErrorResponse{
                    fmt::format("Failed to store final segment: {}", flush_res.error().message), k500InternalServerError});
            }
            existing_segs.emplace(seg_idx, flush_res.value());
            state->seg_buf.clear();
        }

        if (existing_segs.empty()) {
            removeUploadState(upload_id);
            co_return createTypedResponse(BackendErrorResponse{"No segments produced", k500InternalServerError});
        }

        // -- 5. Finalize SHA-512 hash over original bytes
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash_bytes{};
        unsigned int hash_len = 0;
        if (EVP_DigestFinal_ex(state->hash_ctx, hash_bytes.data(), &hash_len) != 1) {
            removeUploadState(upload_id);
            co_return createTypedResponse(BackendErrorResponse{"Failed to finalize file hash", k500InternalServerError});
        }
        std::string file_hash = sgrn::utils::encoding::toBase64Url(hash_bytes.data(), hash_len);
        const int64_t original_bytes = state->original_bytes;
        const bool server_compressed = state->server_compressed;
        removeUploadState(upload_id);

        // Integrity: a retried/duplicated chunk or a lost stream state shows up here.
        if (original_bytes != declared_size) {
            co_await db->execSqlCoro("UPDATE storage.upload_sessions SET status = 'aborted', updated_at = NOW() WHERE id = $1", upload_id);
            co_return createTypedResponse(BackendErrorResponse{
                fmt::format("Size mismatch: received {} bytes, declared {}", original_bytes, declared_size), k422UnprocessableEntity});
        }

        std::string ext(sgrn::utils::strings::extractExtension(filename));

        // -- 5b. Content sniffing (skipped when the server compressed the payload)
        std::string sniffed_ext;
        if (!server_compressed) {
            auto s3_res2 = storage_service_.S3Client();
            if (s3_res2.has_value()) {
                auto* s3_sniff = s3_res2.value();
                std::string first_key = existing_segs.empty() ? "" : "pending/" + upload_id + "/0";
                if (!first_key.empty()) {
                    auto content_res = co_await s3_sniff->getObjectResponse(bucket, first_key, 4096);
                    if (content_res.has_value()) {
                        std::string_view preview = content_res->body.substr(0, std::min<size_t>(content_res->body.size(), 4096));
                        auto sniff_res = sgrn::utils::file_signature::sniffExtension(preview);
                        if (sniff_res.has_value()) {
                            sniffed_ext = *sniff_res;
                        }
                    }
                }
            }
        }

        // -- 5c. Validate sniffed extension against declared extension
        if (!sniffed_ext.empty()) {
            std::string declared_lower = ext;
            std::transform(declared_lower.begin(), declared_lower.end(), declared_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string sniffed_lower = sniffed_ext;
            std::transform(sniffed_lower.begin(), sniffed_lower.end(), sniffed_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!sgrn::utils::file_signature::isCompatibleSniffedFormat(declared_lower, sniffed_lower)) {
                auto format_res = co_await services::storage::helpers::getFormat(db, sniffed_lower);
                if (format_res.has_value()) {
                    co_return createTypedResponse(
                        BackendErrorResponse{fmt::format("Content type mismatch: file content appears to be '{}' but was declared as '{}'",
                                                 sniffed_lower, declared_lower),
                            k422UnprocessableEntity});
                }
            }
        }

        // -- 6. Prepare final objects (encrypt if policy requested, else dedup & rename)
        std::vector<std::pair<int32_t, int64_t>> final_segments;
        std::string final_key_id;
        std::string final_obj_key;

        if (is_encrypted) {
            auto key_store = sgrn::datastore::crypto::getKeyStore();
            if (!key_store)
                co_return createTypedResponse(BackendErrorResponse{"Key store is not initialized", k500InternalServerError});

            // Ciphertext must NEVER share the content-addressed "uploads/<hash>/" namespace.
            final_obj_key = "enc/" + upload_id + "/0";

            auto pending_res = key_store->createPending(final_obj_key);
            CO_RETURN_IF(pending_res.hasError(),
                createTypedResponse(BackendErrorResponse{"Failed to create key record: " + pending_res.error(), k500InternalServerError}));
            auto key_rec = std::move(pending_res.value());
            final_key_id = key_rec.key_id;

            co_await db->execSqlCoro("UPDATE storage.upload_sessions SET pending_key_id = $1 WHERE id = $2", final_key_id, upload_id);

            std::string payload_to_encrypt;
            for (const auto& [old_seg_idx, old_obj_id] : existing_segs) {
                std::string prov_key = "pending/" + upload_id + "/" + std::to_string(old_seg_idx);
                auto get_res = co_await s3->getObjectContent(bucket, prov_key);
                if (get_res.hasError()) {
                    (void)key_store->shred(final_key_id);
                    co_return createTypedResponse(BackendErrorResponse{"Failed to read segment for encryption", k500InternalServerError});
                }
                payload_to_encrypt.append(*get_res);
            }

            std::span<const uint8_t> plain_span(reinterpret_cast<const uint8_t*>(payload_to_encrypt.data()), payload_to_encrypt.size());
            auto enc_res = sgrn::utils::crypto::encryptBuffer(plain_span, key_rec.dek, final_key_id, key_rec.chunk_size);
            if (enc_res.hasError()) {
                (void)key_store->shred(final_key_id);
                co_return createTypedResponse(BackendErrorResponse{"Encryption failed: " + enc_res.error(), k500InternalServerError});
            }
            const auto& ciphertext = enc_res.value();

            auto put_res = co_await s3->uploadFromMemory(bucket, final_obj_key,
                std::string(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size()), "application/octet-stream");
            if (put_res.hasError()) {
                (void)key_store->shred(final_key_id);
                co_return createTypedResponse(BackendErrorResponse{"Failed to upload ciphertext to storage", k500InternalServerError});
            }

            for (const auto& [old_seg_idx, old_obj_id] : existing_segs) {
                std::string prov_key = "pending/" + upload_id + "/" + std::to_string(old_seg_idx);
                co_await s3->deleteFile(bucket, prov_key);
                co_await db->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", old_obj_id);
            }

            std::string ct_sha;
            auto sha_res =
                sgrn::utils::computeSha512Data(std::string_view(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size()),
                    sgrn::utils::HashEncoding::base64url);
            if (sha_res.hasValue()) {
                ct_sha = sha_res.value();
            }
            // Record the zstd layer so the read path knows to decompress after decrypting.
            std::optional<std::string> comp_alg = server_compressed ? std::optional<std::string>("zstd") : std::nullopt;
            std::optional<int32_t> comp_lvl = server_compressed ? std::optional<int32_t>(kCompressionLevel) : std::nullopt;
            auto obj_res = co_await db->execSqlCoro(
                "INSERT INTO storage.objects (bucket, key, size, original_size, sha512, is_compressed, compression_algorithm, "
                "compression_level, upload_mode) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 'single') RETURNING id",
                bucket, final_obj_key, static_cast<int64_t>(ciphertext.size()), original_bytes, ct_sha, server_compressed, comp_alg,
                comp_lvl);
            final_segments.emplace_back(0, sgrn::orm::fromRowByName<ObjectIdRow>(obj_res[0]).id);
        } else {
            int32_t part_idx = 0;
            for (const auto& [old_seg_idx, old_obj_id] : existing_segs) {
                std::string prov_key = "pending/" + upload_id + "/" + std::to_string(old_seg_idx);
                std::string final_key = "uploads/" + file_hash + "/" + std::to_string(part_idx);

                auto dedup_rows =
                    co_await db->execSqlCoro("SELECT id FROM storage.objects WHERE bucket = $1 AND key = $2", bucket, final_key);

                int64_t final_obj_id = 0;
                if (!dedup_rows.empty()) {
                    final_obj_id = sgrn::orm::fromRowByName<ObjectIdRow>(dedup_rows[0]).id;
                    co_await s3->deleteFile(bucket, prov_key);
                    co_await db->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", old_obj_id);
                } else {
                    auto copy_res = co_await s3->copyFile(bucket, prov_key, bucket, final_key);
                    CO_RETURN_IF(copy_res.hasError(),
                        createTypedResponse(BackendErrorResponse{
                            fmt::format("Failed to rename segment {}: {}", part_idx, copy_res.error().message), k500InternalServerError}));
                    co_await s3->deleteFile(bucket, prov_key);
                    co_await db->execSqlCoro(
                        "UPDATE storage.objects SET key = $1, sha512 = $2 WHERE id = $3", final_key, file_hash, old_obj_id);
                    final_obj_id = old_obj_id;
                }
                co_await db->execSqlCoro("UPDATE storage.objects SET sha512 = $1 WHERE id = $2", file_hash, final_obj_id);
                final_segments.emplace_back(part_idx, final_obj_id);
                ++part_idx;
            }
        }

        // -- 7. Write DB records (only after all S3 operations confirmed)
        std::string virtual_file_path;
        if (target_path.empty() || target_path == "/") {
            virtual_file_path = "/" + filename;
        } else {
            virtual_file_path = target_path;
            if (virtual_file_path.back() != '/')
                virtual_file_path += '/';
            virtual_file_path += filename;
        }

        auto dir_res = co_await services::storage::helpers::resolveDirectoryPath(
            db, owner_user, owner_service, owner_session, virtual_file_path, domain_str);
        std::optional<int64_t> directory_id = (dir_res.hasValue() && dir_res.value().has_value()) ? dir_res.value() : std::nullopt;

        auto file_res = co_await services::storage::helpers::insertFile(db, filename, final_segments[0].second, owner_user, owner_service,
            owner_session, ext, directory_id, domain_str, is_encrypted ? std::optional<std::string>(final_key_id) : std::nullopt,
            is_encrypted, services::storage::currentStorageConfig().versioning_enabled);
        if (file_res.hasError()) {
            if (is_encrypted)
                co_await rollbackEncryptedUpload(s3, db, bucket, final_obj_key, final_key_id, final_segments[0].second, std::nullopt);
            const BackendError& err = file_res.error();
            if (err.kind == BackendErrorKind::AlreadyExists)
                co_return createTypedResponse(BackendErrorResponse{"A file with that name already exists in this location.", k409Conflict});
            co_return createTypedResponse(BackendErrorResponse{err.message, k500InternalServerError});
        }
        const int64_t file_id = file_res.value();

        if (is_encrypted) {
            auto ks = sgrn::datastore::crypto::getKeyStore();
            if (!ks) {
                co_await rollbackEncryptedUpload(s3, db, bucket, final_obj_key, final_key_id, final_segments[0].second, file_id);
                co_return createTypedResponse(BackendErrorResponse{"Key store is not initialized", k500InternalServerError});
            }
            auto act_res = ks->activate(final_key_id, std::to_string(file_id));
            if (act_res.hasError()) {
                // A file whose key never activates is undecryptable: do not leave it behind.
                SGRN_ERROR("StorageUpload", "Key activation failed for key {}: {}", final_key_id, act_res.error());
                co_await rollbackEncryptedUpload(s3, db, bucket, final_obj_key, final_key_id, final_segments[0].second, file_id);
                co_return createTypedResponse(
                    BackendErrorResponse{fmt::format("Failed to activate encryption key: {}", act_res.error()), k500InternalServerError});
            }
        }

        const std::string seg_role = (final_segments.size() == 1) ? "primary" : "part";
        for (const auto& [pidx, obj_id] : final_segments) {
            co_await db->execSqlCoro("INSERT INTO storage.file_objects (file_id, object_id, part_index, role) "
                                     "VALUES ($1, $2, $3, $4) ON CONFLICT (file_id, part_index) DO UPDATE SET object_id = "
                                     "EXCLUDED.object_id, role = EXCLUDED.role",
                file_id, obj_id, pidx, seg_role);
        }

        const auto version_cfg = services::storage::currentStorageConfig();
        if (version_cfg.versioning_enabled) {
            auto version_res = co_await services::storage::commitFileVersion(db, file_id, file_hash, original_bytes, is_encrypted,
                is_encrypted ? std::optional<std::string>(final_key_id) : std::nullopt, owner_user, owner_service,
                version_cfg.versioning_snapshot_interval);

            CO_RETURN_IF(version_res.hasError(), createTypedResponse(BackendErrorResponse{version_res.error()}));
        }

        // -- 8. Mark session completed
        co_await db->execSqlCoro("UPDATE storage.upload_sessions "
                                 "SET status = 'completed', file_hash = $2, "
                                 "    compressed_size = (SELECT COALESCE(SUM(o.size), 0) "
                                 "                       FROM storage.file_objects fo "
                                 "                       JOIN storage.objects o ON o.id = fo.object_id "
                                 "                       WHERE fo.file_id = $3), "
                                 "    updated_at = NOW() WHERE id = $1",
            upload_id, file_hash, file_id);

        co_return createTypedResponse(tsp_req,
            UploadCompleteResponse{
                .upload_id = upload_id,
                .file_id = file_id,
                .file_hash = std::move(file_hash),
                .segment_count = final_segments.size(),
                .original_bytes = original_bytes,
                .is_encrypted = is_encrypted,
                .message = "Upload completed successfully",
            },
            k200OK);
    } catch (const std::exception& e) {
        removeUploadState(upload_id);
        ERROR_LOG("Complete upload error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{"Failed completing upload", k500InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleAbortUploadSession(HttpRequestPtr tsp_req) {

    DEBUG_HTTP_REQUEST(tsp_req, "handleAbortUploadSession");

    std::string upload_id = uploadIdFromRequest(tsp_req, UploadIdSource::QueryOrJsonBody);

    CO_RETURN_IF(upload_id.empty(), createTypedResponse(BackendErrorResponse{"upload_id is required", k400BadRequest}));

    try {
        auto db_res = sgrn::datastore::core::getDbClient();

        CO_RETURN_IF(db_res.hasError(), createTypedResponse(BackendErrorResponse{db_res.error()}));

        auto db = db_res.value();

        const SessionActor actor = sessionActor(tsp_req);

        auto res = co_await db->execSqlCoro("UPDATE storage.upload_sessions SET status = 'aborted', updated_at = NOW() WHERE id = $1 "
                                            "AND (user_id = $2 OR automated_service_id = $3)",
            upload_id, actor.user_id, actor.service_id);

        CO_RETURN_IF(res.affectedRows() == 0, createTypedResponse(BackendErrorResponse{"Upload session not found", k404NotFound}));

        co_return createTypedResponse(tsp_req, UploadAbortResponse{"Upload session aborted"}, k200OK);

    } catch (const std::exception& e) {
        ERROR_LOG("Abort upload error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{"Failed aborting upload", k500InternalServerError});
    }
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
