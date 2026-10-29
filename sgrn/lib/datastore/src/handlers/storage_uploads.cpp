#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/helpers/magic.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
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
#include <zstd.h>

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

// storage_uploads.cpp — chunked upload session state machine
// (init/upload/status/complete/abort; split out of storage.cpp).

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleInitUploadSession(drogon::HttpRequestPtr tsp_req) {
    auto p_json = tsp_req->getJsonObject();
    if (!p_json) {
        co_return createJsonErrorResponse("Invalid JSON payload", k400BadRequest);
    }

    std::string filename = p_json->get("filename", "").asString();
    std::string target_path = p_json->get("target_path", "/").asString();
    std::string mime_type = p_json->get("mime_type", "application/octet-stream").asString();
    int64_t total_size = p_json->get("total_size", 0).asInt64();
    int64_t chunk_size = 0;
    if (p_json->isMember("chunk_size") && (*p_json)["chunk_size"].asInt64() > 0) {
        chunk_size = (*p_json)["chunk_size"].asInt64();
    } else {
        // Adaptive chunk negotiation based on file size:
        // <= 50MB: 5MB chunks (<= 10 HTTP requests)
        // 50MB - 500MB: 8MB chunks (6 to 63 HTTP requests)
        // 500MB - 5GB: 16MB chunks (31 to 312 HTTP requests)
        // > 5GB: 32MB chunks
        if (total_size > static_cast<int64_t>(5ULL * 1024ULL * 1024ULL * 1024ULL)) {
            chunk_size = 32 * 1024 * 1024;
        } else if (total_size > 500 * 1024 * 1024) {
            chunk_size = 16 * 1024 * 1024;
        } else if (total_size > 50 * 1024 * 1024) {
            chunk_size = 8 * 1024 * 1024;
        } else {
            chunk_size = 5 * 1024 * 1024;
        }
    }

    if (filename.empty() || total_size <= 0 || chunk_size <= 0) {
        co_return createJsonErrorResponse("filename, total_size, and chunk_size are required", k400BadRequest);
    }

    int32_t total_chunks = static_cast<int32_t>((total_size + chunk_size - 1) / chunk_size);
    std::string upload_id = drogon::utils::getUuid();

    const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
    int32_t user_id = session["user"]["id"].asInt();

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // ── Check user constraints (quota, max file size, chunk size) ────────
        auto user_row = co_await db->execSqlCoro(
            "SELECT storage_limit, total_real_size, max_file_size_mb, preferred_chunk_size_mb FROM core.users WHERE id = $1", user_id);
        if (!user_row.empty()) {
            const auto& u = user_row[0];
            if (!u["max_file_size_mb"].isNull()) {
                const int64_t max_bytes = u["max_file_size_mb"].as<int64_t>() * 1024 * 1024;
                if (total_size > max_bytes) {
                    co_return createJsonErrorResponse(
                        fmt::format("File size ({:.1f} MB) exceeds your account maximum allowed file size ({:.1f} MB)",
                            static_cast<double>(total_size) / (1024.0 * 1024.0), static_cast<double>(max_bytes) / (1024.0 * 1024.0)),
                        k400BadRequest);
                }
            }
            if (!u["storage_limit"].isNull()) {
                const int64_t limit_bytes = u["storage_limit"].as<int64_t>();
                const int64_t used_bytes = u["total_real_size"].as<int64_t>();
                if (used_bytes + total_size > limit_bytes) {
                    co_return createJsonErrorResponse(
                        "Storage quota exceeded. Please delete unused files or request a quota increase.", k413RequestEntityTooLarge);
                }
            }
            if (!u["preferred_chunk_size_mb"].isNull() && u["preferred_chunk_size_mb"].as<int32_t>() > 0) {
                chunk_size = static_cast<int64_t>(u["preferred_chunk_size_mb"].as<int32_t>()) * 1024 * 1024;
            }
        }

        // Recalculate total_chunks if chunk_size was adjusted
        total_chunks = static_cast<int32_t>((total_size + chunk_size - 1) / chunk_size);

        co_await db->execSqlCoro(
            "INSERT INTO storage.upload_sessions (id, user_id, target_path, filename, mime_type, total_size, chunk_size, total_chunks) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
            upload_id, user_id, target_path, filename, mime_type, total_size, chunk_size, total_chunks);

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","chunk_size":{},"total_chunks":{},"status":"active"}})", upload_id, chunk_size, total_chunks));
    } catch (const std::exception& e) {
        ERROR_LOG("Init upload session error: {}", e.what());
        co_return createJsonErrorResponse("Failed to initialize upload session", k500InternalServerError);
    }
}

// ── Segment size for the streaming upload pipeline ────────────────────────────
// Chunks received from the client are compressed and buffered; once the
// compressed buffer reaches kSegmentBytes, it is flushed as its own Garage
// object.  8 MiB is a good default: above S3's 5 MiB part minimum, small
// enough to keep memory bounded even for many concurrent uploads.
static constexpr size_t kSegmentBytes = 8ULL * 1024ULL * 1024ULL;
static constexpr uint8_t kCompressionLevel = 3;

// ── UploadSessionState helpers ─────────────────────────────────────────────
std::shared_ptr<StorageApiHandler::UploadSessionState> StorageApiHandler::getOrCreateUploadState(
    const std::string& t_upload_id, const std::string& t_mime_type, uint8_t t_level) {
    std::lock_guard<std::mutex> lock(upload_states_mutex_);
    auto it = upload_states_.find(t_upload_id);
    if (it != upload_states_.end())
        return it->second;

    auto state = std::make_shared<UploadSessionState>();

    // SHA-256 context
    state->hash_ctx = EVP_MD_CTX_new();
    if (!state->hash_ctx)
        return nullptr;
    if (EVP_DigestInit_ex(state->hash_ctx, EVP_sha256(), nullptr) != 1)
        return nullptr;

    // Only compress if the MIME type is compressible (e.g. text/json/xml).
    // For already-compressed or binary formats, store verbatim.
    if (services::storage::helpers::isCompressibleMimeType(t_mime_type)) {
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

// ── Flush one full segment from seg_buf to Garage ─────────────────────────
// Returns the new storage.objects id, or error.
static drogon::Task<sgrn::datastore::BackendResult<int64_t>> flushSegment(sgrn::datastore::plugins::aws::S3Client* s3,
    const drogon::orm::DbClientPtr& db, const std::string& bucket, const std::string& upload_id, int32_t seg_idx,
    std::vector<char>& seg_buf, const std::string& mime_type) {
    using namespace sgrn::datastore;
    using namespace sgrn::datastore::services::storage;

    // Build provisional key: pending/<upload_id>/<seg_idx>
    std::string seg_key = "pending/" + upload_id + "/" + std::to_string(seg_idx);
    std::string seg_data(seg_buf.begin(), seg_buf.end());
    const int64_t compressed_size = static_cast<int64_t>(seg_data.size());

    // Push segment to Garage
    auto put_res = co_await s3->uploadFromMemory(bucket, seg_key, seg_data, mime_type);
    if (put_res.hasError()) {
        co_return put_res.error();
    }

    // original_size for a provisional segment is not known yet (we only know
    // total original_size at complete time). We store compressed_size as a
    // placeholder; it is corrected after the rename in handleCompleteUploadSession.
    // Compute SHA-256 of the compressed segment data as a placeholder.
    auto sha256_res = helpers::computeSha256InMemory(seg_data);
    if (sha256_res.hasError()) {
        co_return sha256_res.error();
    }
    auto obj_res =
        co_await helpers::insertObject(db, bucket, seg_key, static_cast<size_t>(compressed_size), static_cast<size_t>(compressed_size),
            *sha256_res, /*is_compressed=*/true, std::string{"zstd"}, static_cast<std::optional<uint8_t>>(kCompressionLevel));
    co_return obj_res;
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleUploadChunk(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    std::string chunk_idx_str = tsp_req->getParameter("chunk_index");
    if (upload_id.empty() || chunk_idx_str.empty()) {
        co_return createJsonErrorResponse("upload_id and chunk_index parameters are required", k400BadRequest);
    }

    int32_t chunk_index = 0;
    try {
        chunk_index = std::stoi(chunk_idx_str);
    } catch (...) {
        co_return createJsonErrorResponse("chunk_index must be an integer", k400BadRequest);
    }

    const std::string_view body = tsp_req->getBody();
    if (body.empty()) {
        co_return createJsonErrorResponse("Chunk payload is empty", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // ── 1. Load session to get mime_type ────────────────────────────────
        auto s_rows = co_await db->execSqlCoro("SELECT mime_type, status FROM storage.upload_sessions WHERE id = $1", upload_id);
        if (s_rows.empty())
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);
        if (s_rows[0]["status"].as<std::string>() != "active")
            co_return createJsonErrorResponse("Upload session is not active", k400BadRequest);
        const std::string mime_type = s_rows[0]["mime_type"].as<std::string>();

        // ── 2. Get-or-create the per-session streaming state ────────────────
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);
        if (!state) {
            co_return createJsonErrorResponse("Failed to allocate upload stream state", k500InternalServerError);
        }

        auto s3_res = storage_service_.S3Client();
        if (s3_res.hasError())
            co_return createJsonErrorResponse(fmt::format("S3 unavailable: {}", s3_res.error().message_), k500InternalServerError);
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
                    co_return createJsonErrorResponse(
                        fmt::format("Compression error: {}", ZSTD_getErrorName(ret)), k500InternalServerError);
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
                co_return createJsonErrorResponse(
                    fmt::format("Failed to store segment {}: {}", seg_idx, flush_res.error().message_), k500InternalServerError);
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

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","chunk_index":{},"received":true,"segments_flushed":{}}})", upload_id, chunk_index, new_segments.size()));

    } catch (const std::exception& e) {
        ERROR_LOG("Upload chunk error: {}", e.what());
        co_return createJsonErrorResponse("Failed storing chunk", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleGetUploadStatus(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    if (upload_id.empty()) {
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto s_rows = co_await db->execSqlCoro("SELECT filename, target_path, total_size, chunk_size, total_chunks, uploaded_chunks_count, "
                                               "status FROM storage.upload_sessions WHERE id = $1",
            upload_id);

        if (s_rows.empty()) {
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);
        }

        const auto& s = s_rows[0];

        auto idx_rows = co_await db->execSqlCoro(
            "SELECT chunk_index FROM storage.upload_chunks WHERE upload_id = $1 ORDER BY chunk_index ASC", upload_id);
        std::string indices_json = "[";
        bool first_idx = true;
        for (const auto& idx_row : idx_rows) {
            if (!first_idx)
                indices_json += ',';
            first_idx = false;
            indices_json += std::to_string(idx_row["chunk_index"].as<int32_t>());
        }
        indices_json += "]";

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","filename":"{}","target_path":"{}","total_size":{},"chunk_size":{},"total_chunks":{},"uploaded_chunks_count":{},"status":"{}","uploaded_chunk_indices":{}}})",
            upload_id, jsonEscape(s["filename"].as<std::string>()), jsonEscape(s["target_path"].as<std::string>()),
            s["total_size"].as<int64_t>(), s["chunk_size"].as<int64_t>(), s["total_chunks"].as<int32_t>(),
            s["uploaded_chunks_count"].as<int32_t>(), s["status"].as<std::string>(), indices_json));

    } catch (const std::exception& e) {
        ERROR_LOG("Get upload status error: {}", e.what());
        co_return createJsonErrorResponse("Failed fetching upload status", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleCompleteUploadSession(drogon::HttpRequestPtr tsp_req) {
    auto p_json = tsp_req->getJsonObject();
    if (!p_json || !p_json->isMember("upload_id"))
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);

    std::string upload_id = (*p_json)["upload_id"].asString();

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // -- 1. Load and validate session
        auto s_rows =
            co_await db->execSqlCoro("SELECT user_id, total_chunks, uploaded_chunks_count, filename, target_path, total_size, mime_type "
                                     "FROM storage.upload_sessions WHERE id = $1",
                upload_id);
        if (s_rows.empty())
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);

        const auto& s = s_rows[0];
        const int32_t total_chunks = s["total_chunks"].as<int32_t>();
        if (s["uploaded_chunks_count"].as<int32_t>() < total_chunks)
            co_return createJsonErrorResponse("Cannot complete: not all chunks have been uploaded", k400BadRequest);

        const int32_t user_id = s["user_id"].as<int32_t>();
        const std::string filename = s["filename"].as<std::string>();
        const std::string target_path = s["target_path"].as<std::string>();
        const std::string mime_type = s["mime_type"].isNull() ? "application/octet-stream" : s["mime_type"].as<std::string>();

        const Json::Value& auth_session = tsp_req->attributes()->get<Json::Value>("session_json");
        const int64_t session_id = auth_session.isMember("session_id") ? auth_session["session_id"].asInt64() : 0;

        // -- 2. Recover streaming pipeline state
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);
        if (!state)
            co_return createJsonErrorResponse("Failed to recover upload stream state", k500InternalServerError);

        auto s3_res = storage_service_.S3Client();
        if (s3_res.hasError())
            co_return createJsonErrorResponse(fmt::format("S3 unavailable: {}", s3_res.error().message_), k500InternalServerError);
        auto* s3 = s3_res.value();
        const services::storage::StorageConfig live_cfg = services::storage::currentStorageConfig();
        const std::string bucket = live_cfg.default_bucket;

        // -- 3. Collect already-flushed segments from DB
        auto seg_rows = co_await db->execSqlCoro("SELECT uc.object_id, o.key "
                                                 "FROM storage.upload_chunks uc "
                                                 "JOIN storage.objects o ON o.id = uc.object_id "
                                                 "WHERE uc.upload_id = $1 AND uc.object_id IS NOT NULL",
            upload_id);

        std::map<int32_t, int64_t> existing_segs;
        for (const auto& row : seg_rows) {
            std::string key = row["key"].as<std::string>();
            int64_t oid = row["object_id"].as<int64_t>();
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
                    co_return createJsonErrorResponse(
                        fmt::format("Compression flush error: {}", ZSTD_getErrorName(ret)), k500InternalServerError);
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
                co_return createJsonErrorResponse(
                    fmt::format("Failed to store final segment: {}", flush_res.error().message_), k500InternalServerError);
            }
            existing_segs.emplace(seg_idx, flush_res.value());
            state->seg_buf.clear();
        }

        if (existing_segs.empty()) {
            removeUploadState(upload_id);
            co_return createJsonErrorResponse("No segments produced", k500InternalServerError);
        }

        // -- 5. Finalize SHA-256 hash over original bytes
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash_bytes{};
        unsigned int hash_len = 0;
        if (EVP_DigestFinal_ex(state->hash_ctx, hash_bytes.data(), &hash_len) != 1) {
            removeUploadState(upload_id);
            co_return createJsonErrorResponse("Failed to finalize file hash", k500InternalServerError);
        }
        static constexpr char kHexChars[] = "0123456789abcdef";
        std::string file_hash;
        file_hash.reserve(hash_len * 2);
        for (unsigned int i = 0; i < hash_len; ++i) {
            file_hash.push_back(kHexChars[hash_bytes[i] >> 4]);
            file_hash.push_back(kHexChars[hash_bytes[i] & 0x0F]);
        }
        const int64_t original_bytes = state->original_bytes;
        const bool server_compressed = state->server_compressed;
        removeUploadState(upload_id);

        // Extract declared extension from filename for sniffing validation
        std::string ext(services::storage::helpers::extractExtension(filename));

        // -- 5b. Content sniffing: read first 4096 bytes from S3 for magic detection.
        // Skip when the server itself compressed the payload: the bytes on S3 will
        // have a zstd magic header regardless of the declared extension (e.g. json),
        // which is correct and expected — the sniff check would produce a false positive.
        std::string sniffed_ext;
        if (!server_compressed) {
            auto s3_res2 = storage_service_.S3Client();
            if (s3_res2.has_value()) {
                auto* s3_sniff = s3_res2.value();
                // Download first segment to read magic bytes (use existing_segs which has provisional segments)
                std::string first_key = existing_segs.empty() ? "" : "pending/" + upload_id + "/0";
                if (!first_key.empty()) {
                    BackendResult<std::string> content_res = co_await s3_sniff->getObjectContent(bucket, first_key, 4096);
                    if (content_res.has_value()) {
                        std::string_view data(*content_res);
                        std::string preview(data.substr(0, std::min<size_t>(data.size(), 4096)));
                        auto sniff_res = ::sgrn::datastore::services::helpers::sniffExtension(preview);
                        if (sniff_res.has_value()) {
                            sniffed_ext = *sniff_res;
                        }
                    }
                }
            }
        }

        // -- 5c. Validate sniffed extension against declared extension
        if (!sniffed_ext.empty()) {
            // Normalize both to lowercase for comparison
            std::string declared_lower = ext;
            std::transform(declared_lower.begin(), declared_lower.end(), declared_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string sniffed_lower = sniffed_ext;
            std::transform(sniffed_lower.begin(), sniffed_lower.end(), sniffed_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!::sgrn::datastore::services::helpers::isCompatibleSniffedFormat(declared_lower, sniffed_lower)) {
                // Check if sniffed extension is a known format in registry
                auto format_res = co_await services::storage::helpers::getFormat(db, sniffed_lower);
                if (format_res.has_value()) {
                    // Sniffed type is a known format but differs from declared → reject
                    co_return createJsonErrorResponse(
                        fmt::format("Content type mismatch: file content appears to be '{}' but was declared as '{}'", sniffed_lower,
                            declared_lower),
                        k422UnprocessableEntity);
                }
                // Sniffed type unknown → fall back to declared extension (registry will handle)
            }
        }

        // -- 6. Rename provisional keys to final content-addressed keys
        // Final key: uploads/<file_hash>/<part_index>
        std::vector<std::pair<int32_t, int64_t>> final_segments;
        int32_t part_idx = 0;
        for (const auto& [old_seg_idx, old_obj_id] : existing_segs) {
            std::string prov_key = "pending/" + upload_id + "/" + std::to_string(old_seg_idx);
            std::string final_key = "uploads/" + file_hash + "/" + std::to_string(part_idx);

            auto dedup_rows = co_await db->execSqlCoro("SELECT id FROM storage.objects WHERE bucket = $1 AND key = $2", bucket, final_key);

            int64_t final_obj_id = 0;
            if (!dedup_rows.empty()) {
                // Deduplicated: reuse existing object
                final_obj_id = dedup_rows[0]["id"].as<int64_t>();
                co_await s3->deleteFile(bucket, prov_key);
                co_await db->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", old_obj_id);
            } else {
                auto copy_res = co_await s3->copyFile(bucket, prov_key, bucket, final_key);
                if (copy_res.hasError())
                    co_return createJsonErrorResponse(
                        fmt::format("Failed to rename segment {}: {}", part_idx, copy_res.error().message_), k500InternalServerError);
                co_await s3->deleteFile(bucket, prov_key);
                co_await db->execSqlCoro(
                    "UPDATE storage.objects SET key = $1, sha256 = $2 WHERE id = $3", final_key, file_hash, old_obj_id);
                final_obj_id = old_obj_id;
            }
            // Update sha256 for deduplicated objects too
            co_await db->execSqlCoro("UPDATE storage.objects SET sha256 = $1 WHERE id = $2", file_hash, final_obj_id);
            final_segments.emplace_back(part_idx, final_obj_id);
            ++part_idx;
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

        auto dir_res =
            co_await services::storage::helpers::resolveDirectoryPath(db, user_id, std::nullopt, session_id, virtual_file_path, "");
        std::optional<int64_t> directory_id = (dir_res.hasValue() && dir_res.value().has_value()) ? dir_res.value() : std::nullopt;

        // Compression-aware suffix (same rule as the single-upload path);
        // insertFile() stores it only when the formats registry acknowledges
        // it, otherwise the extension stays NULL and the suffix stays part of
        // the name. (ext already extracted earlier for sniffing validation)
        // std::string ext(services::storage::helpers::extractExtension(filename));

        auto file_res = co_await services::storage::helpers::insertFile(
            db, filename, final_segments[0].second, user_id, std::nullopt, session_id, ext, directory_id, "");
        if (file_res.hasError()) {
            const BackendError& err = file_res.error();
            if (err.kind_ == BackendErrorKind::AlreadyExists)
                co_return createJsonErrorResponse("A file with that name already exists in this location.", k409Conflict);
            co_return createJsonErrorResponse(err.message_, k500InternalServerError);
        }
        const int64_t file_id = file_res.value();

        // Insert ordered segment manifest
        const std::string seg_role = (final_segments.size() == 1) ? "primary" : "part";
        for (const auto& [pidx, obj_id] : final_segments) {
            co_await db->execSqlCoro("INSERT INTO storage.file_objects (file_id, object_id, part_index, role) "
                                     "VALUES ($1, $2, $3, $4) ON CONFLICT (file_id, part_index) DO NOTHING",
                file_id, obj_id, pidx, seg_role);
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

        co_return createJsonResponse(fmt::format(
            R"({{"success":true,"upload_id":"{}","file_id":{},"file_hash":"{}","segment_count":{},"original_bytes":{},"message":"Upload completed successfully"}})",
            upload_id, file_id, file_hash, final_segments.size(), original_bytes));

    } catch (const std::exception& e) {
        removeUploadState(upload_id);
        ERROR_LOG("Complete upload error: {}", e.what());
        co_return createJsonErrorResponse("Failed completing upload", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleAbortUploadSession(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    if (upload_id.empty()) {
        auto p_json = tsp_req->getJsonObject();
        if (p_json && p_json->isMember("upload_id")) {
            upload_id = (*p_json)["upload_id"].asString();
        }
    }

    if (upload_id.empty()) {
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        co_await db->execSqlCoro("UPDATE storage.upload_sessions SET status = 'aborted', updated_at = NOW() WHERE id = $1", upload_id);

        co_return createJsonResponse(R"({"success":true,"message":"Upload session aborted"})");

    } catch (const std::exception& e) {
        ERROR_LOG("Abort upload error: {}", e.what());
        co_return createJsonErrorResponse("Failed aborting upload", k500InternalServerError);
    }
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
