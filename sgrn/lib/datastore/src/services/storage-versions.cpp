// src/services/storage-versions.cpp
//
// Version content loading, compaction, restore, and retention.

#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/services/versioning-delta.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/crypto/stream.hpp>
#include <sgrn/utils/delta.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/mime.hpp>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <trantor/net/EventLoopThread.h>

namespace sgrn::datastore::services::storage
{
using drogon::Task;
using drogon::orm::DbClientPtr;

namespace delta = sgrn::utils::delta;

using sgrn::datastore::plugins::aws::S3Client;

namespace
{
std::string sha512Hex(std::string_view t_data) {

    auto r = sgrn::utils::computeSha512Data(t_data, sgrn::utils::HashEncoding::base64url);

    RETURN_IF(r.hasError(), std::string{});

    return std::string(r.value());
}

std::span<const uint8_t> asBytes(const std::string& t_s) {
    return {reinterpret_cast<const uint8_t*>(t_s.data()), t_s.size()};
}

template <typename Bytes>
std::string toString(const Bytes& t_b) {
    return std::string(reinterpret_cast<const char*>(t_b.data()), t_b.size());
}

// Heavy CPU (zstd, hashing big buffers) runs here, never on a Drogon IO loop.
trantor::EventLoop* compactorLoop() {
    static trantor::EventLoopThread thread("sgrn-compactor");
    static const bool started = (thread.run(), true);
    (void)started;
    return thread.getLoop();
}

Result<std::string, BackendError> decryptWithKey(const std::string& t_cipher, const std::string& t_key_id) {
    auto ks = sgrn::datastore::crypto::getKeyStore();

    RETURN_ERROR_IF(!ks, BackendError(BackendErrorKind::Runtime, "Key store unavailable"));

    auto key = ks->get(t_key_id);

    RETURN_ERROR_IF(key.hasError(), BackendError(BackendErrorKind::Runtime, "Version key unavailable (shredded?)"));

    auto dec = sgrn::utils::crypto::decryptBuffer(asBytes(t_cipher), key.value().dek, t_key_id);

    RETURN_ERROR_IF(dec.hasError(), BackendError(BackendErrorKind::Runtime, "Decryption failed"));

    return toString(dec.value());
}

Result<std::string, BackendError> encryptWithKey(const std::string& t_plain, const std::string& t_key_id) {

    auto ks = sgrn::datastore::crypto::getKeyStore();

    RETURN_ERROR_IF(!ks, BackendError(BackendErrorKind::Runtime, "Key store unavailable"));

    auto key = ks->get(t_key_id);

    RETURN_ERROR_IF(key.hasError(), BackendError(BackendErrorKind::Runtime, "Version key unavailable (shredded?)"));

    auto enc = sgrn::utils::crypto::encryptBuffer(asBytes(t_plain), key.value().dek, t_key_id, key.value().chunk_size);

    RETURN_ERROR_IF(enc.hasError(), BackendError(BackendErrorKind::Runtime, "Encryption failed"));

    return toString(enc.value());
}
} // namespace

// ============================================================================
// Content loading
// ============================================================================

// Logical bytes (decrypted + decompressed) of a version that is stored in FULL.
Task<Result<std::string, BackendError>> loadFullVersionBytes(DbClientPtr t_db, S3Client* t_s3, const std::string& t_bucket,
    int64_t t_version_id, bool t_is_encrypted, const std::optional<std::string>& t_key_id, size_t t_original_size) {
    try {
        auto rows = co_await t_db->execSqlCoro("SELECT o.key, o.is_compressed, o.compression_algorithm "
                                               "FROM storage.version_objects vo JOIN storage.objects o ON o.id = vo.object_id "
                                               "WHERE vo.version_id = $1 ORDER BY vo.part_index",
            t_version_id);

        CO_RETURN_ERROR_IF(rows.empty(), BackendError(BackendErrorKind::Runtime, "Version has no stored objects"));

        std::string bytes;
        for (const auto& row : rows) {
            auto part = co_await t_s3->getObjectContent(t_bucket, row["key"].as<std::string>());

            CO_RETURN_ERROR_IF(part.hasError(), BackendError(BackendErrorKind::Runtime, "Failed to read version object from storage"));

            bytes.append(*part);
        }
        if (t_is_encrypted) {

            CO_RETURN_ERROR_IF(!t_key_id, BackendError(BackendErrorKind::Runtime, "Encrypted version has no key"));

            auto dec = decryptWithKey(bytes, *t_key_id);

            CO_RETURN_ERROR_IF(dec.hasError(), dec.error());

            bytes = std::move(dec.value());
        }
        const bool compressed = rows[0]["is_compressed"].as<bool>();
        if (compressed) {
            const std::string algo = rows[0]["compression_algorithm"].isNull() ? "" : rows[0]["compression_algorithm"].as<std::string>();

            CO_RETURN_ERROR_IF(algo != "zstd", BackendError(BackendErrorKind::Runtime, "Unsupported compression for versioning: " + algo));

            auto inflated = sgrn::utils::compression::decompressStringZstd(bytes, t_original_size);

            CO_RETURN_ERROR_IF(inflated.hasError(), BackendError(BackendErrorKind::Runtime, "Inflate failed: " + inflated.error()));

            bytes = std::move(inflated.value());
        }
        co_return bytes;
    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, ex.what());
    }
}

// Logical bytes of ANY version: loads the full base then replays reverse deltas.
Task<Result<std::string, BackendError>> loadVersionContent(
    DbClientPtr t_db, S3Client* t_s3, const std::string& t_bucket, int64_t t_version_id) {
    try {
        auto plan = co_await t_db->execSqlCoro(
            "SELECT step, version_id, storage_kind, delta_object_key, delta_is_encrypted, delta_key_id, original_size, content_sha512 "
            "FROM storage.version_plan($1)",
            t_version_id);
        if (plan.empty()) {
            co_return BackendError(BackendErrorKind::Runtime, "Version not found");
        }

        auto base_meta = co_await t_db->execSqlCoro(
            "SELECT is_encrypted, key_id FROM storage.file_versions WHERE id = $1", plan[0]["version_id"].as<int64_t>());
        std::optional<std::string> base_key;
        if (!base_meta[0]["key_id"].isNull()) {
            base_key = base_meta[0]["key_id"].as<std::string>();
        }
        auto cur = co_await loadFullVersionBytes(t_db, t_s3, t_bucket, plan[0]["version_id"].as<int64_t>(),
            base_meta[0]["is_encrypted"].as<bool>(), base_key, static_cast<size_t>(plan[0]["original_size"].as<int64_t>()));

        CO_IF_ERROR_PROPAGATE(cur);

        std::string content = std::move(cur.value());

        for (size_t i = 0; i + 1 < plan.size(); ++i) {
            auto blob = co_await t_s3->getObjectContent(t_bucket, plan[i]["delta_object_key"].as<std::string>());
            if (blob.hasError()) {
                co_return BackendError(BackendErrorKind::Runtime, "Failed to read delta from storage");
            }
            std::string delta = *blob;
            if (plan[i]["delta_is_encrypted"].as<bool>()) {
                auto dec = decryptWithKey(delta, plan[i]["delta_key_id"].as<std::string>());
                if (dec.hasError()) {
                    co_return dec.error();
                }
                delta = std::move(dec.value());
            }
            std::string next, err;
            const auto expected = static_cast<size_t>(plan[i + 1]["original_size"].as<int64_t>());
            const Result<void, std::string> result = co_await drogon::queueInLoopCoro<Result<void, std::string>>(compactorLoop(),
                [&] { return delta::applyDelta(content, delta, expected, next); }); // SEAM: queueInLoopCoro (Drogon >= 1.8)

            CO_RETURN_ERROR_IF(result.hasError(), BackendError(BackendErrorKind::Runtime, "Delta replay failed: " + result.error()));

            content = std::move(next);
        }

        if (plan.size() > 1) { // integrity check whenever at least one delta was applied
            const std::string want = plan[plan.size() - 1]["content_sha512"].as<std::string>();

            CO_RETURN_IF(sha512Hex(content) != want, BackendError(BackendErrorKind::Runtime, "Version integrity check failed"));
        }
        co_return content;
    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, ex.what());
    }
}

// ============================================================================
// Compactor: turns "full, non-head, non-checkpoint" versions into reverse deltas
// ============================================================================

// After attach_delta / trim, delete Garage blobs whose storage.objects row is gone.
Task<void> deleteBlobsIfRowGone(DbClientPtr t_db, S3Client* t_s3, const std::string& t_bucket, const std::vector<std::string>& t_keys) {
    for (const auto& key : t_keys) {
        auto still = co_await t_db->execSqlCoro("SELECT 1 FROM storage.objects WHERE bucket = $1 AND key = $2", t_bucket, key);
        if (still.empty()) {
            co_await t_s3->deleteFile(t_bucket, key);
        }
    }
}

// Returns true when a delta was stored, false when the version was kept full.
Task<Result<versioning::DeltaStorageStatus, BackendError>> compactVersion(
    DbClientPtr t_db, S3Client* t_s3, const StorageConfig& t_cfg, int64_t t_base_id, int64_t t_target_id) {

    using versioning::DeltaStorageStatus;

    try {
        const std::string bucket = t_cfg.default_bucket;
        // stored_size is what the full version actually occupies at rest
        // (compressed and/or encrypted), which is the number a delta must beat.
        auto meta = co_await t_db->execSqlCoro("SELECT fv.file_id, fv.original_size, fv.content_sha512, fv.is_encrypted, fv.key_id, "
                                               "coalesce((SELECT sum(o.size) FROM storage.version_objects vo "
                                               "          JOIN storage.objects o ON o.id = vo.object_id "
                                               "          WHERE vo.version_id = fv.id), 0)::bigint AS stored_size "
                                               "FROM storage.file_versions fv "
                                               "WHERE fv.id = $1 AND fv.storage_kind = 'full' AND NOT fv.is_checkpoint",
            t_target_id);

        CO_RETURN_IF(meta.empty(), DeltaStorageStatus::DeltaWasNotStored); // raced with another worker / already handled

        const int64_t file_id = meta[0]["file_id"].as<int64_t>();
        const auto original_size = static_cast<size_t>(meta[0]["original_size"].as<int64_t>());
        const std::string target_sha = meta[0]["content_sha512"].as<std::string>();
        const bool enc = meta[0]["is_encrypted"].as<bool>();
        std::optional<std::string> key_id;
        if (!meta[0]["key_id"].isNull()) {
            key_id = meta[0]["key_id"].as<std::string>();
        }

        auto keep_full = [&]() -> Task<void> {
            co_await t_db->execSqlCoro("UPDATE storage.file_versions SET is_checkpoint = true WHERE id = $1", t_target_id);
        };
        if (original_size > t_cfg.deltaMaxBytes()) {
            co_await keep_full();
            co_return DeltaStorageStatus::DeltaWasNotStored;
        }

        auto target = co_await loadVersionContent(t_db, t_s3, bucket, t_target_id);

        CO_IF_ERROR_PROPAGATE(target);

        if (sha512Hex(target.value()) != target_sha) { // metadata does not describe the stored bytes: never delta it
            co_await keep_full();
            co_return DeltaStorageStatus::DeltaWasNotStored;
        }

        auto base = co_await loadVersionContent(t_db, t_s3, bucket, t_base_id);

        CO_IF_ERROR_PROPAGATE(base);

        std::string delta;

        const auto result = co_await drogon::queueInLoopCoro<Result<void, std::string>>(
            compactorLoop(), [&] { return delta::makeDelta(base.value(), target.value(), t_cfg.delta_zstd_level, delta); });

        CO_RETURN_IF(result.hasError(), BackendError(BackendErrorKind::Runtime, "Delta generation failed: " + result.error()));

        // Not worth it (already-compressed / unrelated content): keep the full copy.
        // Compare against the bytes the full version actually occupies, not the
        // logical size: a delta that only beats the uncompressed target can
        // still be larger than the compressed object that would replace it.
        const auto stored_size_json = meta[0]["stored_size"];
        const uint64_t stored_size = stored_size_json.isNull() ? 0 : static_cast<uint64_t>(stored_size_json.as<int64_t>());
        const uint64_t full_size = stored_size > 0 ? stored_size : static_cast<uint64_t>(target.value().size());
        if (static_cast<uint64_t>(delta.size()) * 100 > full_size * (100 - t_cfg.delta_min_saving_pct)) {
            co_await keep_full();
            co_return DeltaStorageStatus::DeltaWasNotStored;
        }
        // Prove the delta round-trips BEFORE the full copy is released.
        std::string rebuilt;

        const auto result_delta = co_await drogon::queueInLoopCoro<Result<void, std::string>>(
            compactorLoop(), [&] { return delta::applyDelta(base.value(), delta, target.value().size(), rebuilt); });

        CO_RETURN_IF(result_delta.hasError() || sha512Hex(rebuilt) != target_sha,
            BackendError(BackendErrorKind::Runtime, "Delta verification failed: " + result_delta.error()));

        const int64_t plain_delta_size = static_cast<int64_t>(delta.size());

        std::string blob = std::move(delta);

        if (enc) { // delta is encrypted with the key of the version it PRODUCES
            auto sealed = encryptWithKey(blob, *key_id);

            CO_IF_ERROR_PROPAGATE(sealed);

            blob = std::move(sealed.value());
        }
        const std::string blob_sha = sha512Hex(blob);
        const std::string blob_key = fmt::format("deltas/{}/{}-{}", file_id, t_target_id, blob_sha.substr(0, 16));

        // Keys of the full copy we are about to release (for blob deletion afterwards).
        std::vector<std::string> freed_keys;
        for (const auto& r : co_await t_db->execSqlCoro(
                 "SELECT o.key FROM storage.version_objects vo JOIN storage.objects o ON o.id = vo.object_id WHERE vo.version_id = $1",
                 t_target_id)) {
            freed_keys.push_back(r["key"].as<std::string>());
        }

        auto put = co_await t_s3->uploadFromMemory(bucket, blob_key, blob, "application/octet-stream");

        CO_RETURN_ERROR_IF(put.hasError(), BackendError(BackendErrorKind::Runtime, "Failed to upload delta"));

        std::optional<std::string> attach_error;
        try {
            // One statement = atomic: register the blob AND flip the version to 'delta'.
            co_await t_db->execSqlCoro("WITH o AS (SELECT storage.upsert_object($1, $2, $3::bigint, $4::bigint, $5::text) AS id) "
                                       "SELECT storage.attach_delta($6, $7, o.id, $5::text, $3::bigint) FROM o",
                bucket, blob_key, static_cast<int64_t>(blob.size()), plain_delta_size, blob_sha, t_base_id, t_target_id);
        } catch (const std::exception& ex) {
            // co_await is illegal inside a catch handler; clean up below instead.
            attach_error = ex.what();
        }
        if (attach_error) {
            co_await t_s3->deleteFile(bucket, blob_key);

            co_return BackendError(BackendErrorKind::Database, std::string("attach_delta failed: ") + *attach_error);
        }

        co_await deleteBlobsIfRowGone(t_db, t_s3, bucket, freed_keys);

        co_return DeltaStorageStatus::DeltaWasStored;

    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, ex.what());
    }
}

Task<void> runCompactionSweep(StorageService& t_svc) {
    static std::atomic<bool> running{false};
    if (running.exchange(true)) {
        co_return;
    }
    struct Guard {
        ~Guard() {
            running = false;
        }
    } guard;

    const StorageConfig cfg = currentStorageConfig();
    if (!cfg.versioning_enabled) {
        co_return;
    }
    auto db = t_svc.getDbClient();
    auto s3 = t_svc.S3Client();
    if (db.hasError() || s3.hasError()) {
        co_return;
    }
    try {
        auto work = co_await db.value()->execSqlCoro(
            "SELECT fv.id AS target_id, nx.id AS base_id "
            "FROM storage.file_versions fv "
            "JOIN storage.files f ON f.id = fv.file_id "
            "JOIN storage.file_versions nx ON nx.file_id = fv.file_id AND nx.version_no = fv.version_no + 1 "
            "WHERE fv.storage_kind = 'full' AND NOT fv.is_checkpoint AND fv.id <> f.current_version_id "
            "ORDER BY fv.created_at LIMIT 20");
        for (const auto& row : work) {
            auto res = co_await compactVersion(db.value(), s3.value(), cfg, row["base_id"].as<int64_t>(), row["target_id"].as<int64_t>());
            if (res.hasError()) {
                SGRN_WARN("StorageVersioning", "Compaction of version {} failed: {}", row["target_id"].as<int64_t>(), res.error().message);
            }
        }
    } catch (const std::exception& ex) {
        SGRN_ERROR("StorageVersioning", "Compaction sweep failed: {}", ex.what());
    }
}

void startVersionCompactor(StorageService& t_svc) {
    static std::once_flag started;
    std::call_once(started, [&t_svc] {
        drogon::app().getLoop()->runEvery(
            5.0, [&t_svc] { drogon::async_run([&t_svc]() -> Task<void> { co_await runCompactionSweep(t_svc); }); });
    });
}

// ============================================================================
// Restore
// ============================================================================

// Creates a NEW head version whose content equals version `t_version_no`.
// History is never rewritten (git-revert semantics).
Task<Result<RestoreResult, BackendError>> restoreVersion(StorageService& t_svc, int64_t t_file_id, int32_t t_version_no,
    bool t_file_encrypted, std::optional<int32_t> t_user_id, std::optional<int32_t> t_service_id, std::optional<std::string> t_comment) {
    try {
        auto db_res = t_svc.getDbClient();
        auto s3_res = t_svc.S3Client();

        CO_RETURN_IF(db_res.hasError() || s3_res.hasError(), BackendError(BackendErrorKind::Runtime, "Storage unavailable"));

        auto db = db_res.value();
        const StorageConfig cfg = currentStorageConfig();

        auto vrow = co_await db->execSqlCoro(
            "SELECT fv.id, fv.storage_kind, fv.content_sha512, fv.original_size, fv.is_encrypted, (fv.id = f.current_version_id) AS "
            "is_current "
            "FROM storage.file_versions fv JOIN storage.files f ON f.id = fv.file_id WHERE fv.file_id = $1 AND fv.version_no = $2",
            t_file_id, t_version_no);

        CO_RETURN_IF(vrow.empty(), BackendError(BackendErrorKind::Generic, "Version not found"));

        const int64_t src_id = vrow[0]["id"].as<int64_t>();
        const std::string sha = vrow[0]["content_sha512"].as<std::string>();
        const int64_t size = vrow[0]["original_size"].as<int64_t>();

        if (vrow[0]["is_current"].as<bool>()) {
            co_return BackendError(BackendErrorKind::AlreadyExists, "Version is already current");
        }

        // The head must be a registered version before its objects get swapped.
        co_await db->execSqlCoro("SELECT storage.snapshot_head_as_version($1, $2, $3)", t_file_id, t_user_id, t_service_id);

        const bool fast_path =
            vrow[0]["storage_kind"].as<std::string>() == "full" && !vrow[0]["is_encrypted"].as<bool>() && !t_file_encrypted;
        if (fast_path) {
            // Zero-copy: point file_objects at the old version's (content-addressed) objects.
            auto tx = co_await db->newTransactionCoro();

            co_await tx->execSqlCoro("DELETE FROM storage.file_objects WHERE file_id = $1", t_file_id);

            co_await tx->execSqlCoro("INSERT INTO storage.file_objects (file_id, object_id, part_index, role) "
                                     "SELECT $1, object_id, part_index, role FROM storage.version_objects WHERE version_id = $2",
                t_file_id, src_id);

            co_await tx->execSqlCoro("UPDATE storage.files SET key_id = NULL, is_encrypted = false WHERE id = $1", t_file_id);

            auto r = co_await tx->execSqlCoro("SELECT * FROM storage.commit_new_version($1,$2,$3,false,NULL,false,$4,$5,$6,$7,$8)",
                t_file_id, sha, size, t_user_id, t_service_id, t_comment, src_id, static_cast<int32_t>(cfg.versioning_snapshot_interval));
            co_return RestoreResult{r[0]["version_id"].as<int64_t>(), r[0]["version_no"].as<int32_t>(), r[0]["created"].as<bool>()};
        }

        // Slow path: rebuild bytes, store them as a fresh head object (re-encrypted when the file is).
        auto content = co_await loadVersionContent(db, s3_res.value(), cfg.default_bucket, src_id);

        CO_IF_ERROR_PROPAGATE(content);

        std::string payload = content.value();
        std::string obj_key;
        std::optional<std::string> new_key_id;
        if (t_file_encrypted) {
            auto ks = sgrn::datastore::crypto::getKeyStore();

            CO_RETURN_ERROR_IF(!ks, BackendError(BackendErrorKind::Runtime, "Key store unavailable"));

            obj_key = "enc/restore-" + drogon::utils::getUuid() + "/0";
            auto pending = ks->createPending(obj_key);

            CO_RETURN_ERROR_IF(pending.hasError(), BackendError(BackendErrorKind::Runtime, "Failed to create key"));

            new_key_id = pending.value().key_id;
            auto sealed =
                sgrn::utils::crypto::encryptBuffer(asBytes(content.value()), pending.value().dek, *new_key_id, pending.value().chunk_size);

            if (sealed.hasError()) {
                (void)ks->shred(*new_key_id);
                co_return BackendError(BackendErrorKind::Runtime, "Encryption failed");
            }

            payload = toString(sealed.value());
        } else {
            obj_key = fmt::format("versions/{}/{}", t_file_id, sha);
        }
        auto put = co_await s3_res.value()->uploadFromMemory(cfg.default_bucket, obj_key, payload, "application/octet-stream");
        if (put.hasError()) {

            if (new_key_id) {
                (void)sgrn::datastore::crypto::getKeyStore()->shred(*new_key_id);
            }
            co_return BackendError(BackendErrorKind::Runtime, "Failed to upload restored content");
        }

        auto tx = co_await db->newTransactionCoro();

        auto obj =
            co_await tx->execSqlCoro("SELECT storage.upsert_object($1, $2, $3::bigint, $4::bigint, $5::text) AS id", cfg.default_bucket,
                obj_key, static_cast<int64_t>(payload.size()), static_cast<int64_t>(content.value().size()), sha512Hex(payload));

        co_await tx->execSqlCoro("DELETE FROM storage.file_objects WHERE file_id = $1", t_file_id);

        co_await tx->execSqlCoro("INSERT INTO storage.file_objects (file_id, object_id, part_index, role) VALUES ($1, $2, 0, 'primary')",
            t_file_id, obj[0]["id"].as<int64_t>());

        co_await tx->execSqlCoro(
            "UPDATE storage.files SET key_id = $2, is_encrypted = $3 WHERE id = $1", t_file_id, new_key_id, new_key_id.has_value());

        auto r = co_await tx->execSqlCoro("SELECT * FROM storage.commit_new_version($1,$2,$3,$4,$5,false,$6,$7,$8,$9,$10)", t_file_id, sha,
            size, new_key_id.has_value(), new_key_id, t_user_id, t_service_id, t_comment, src_id,
            static_cast<int32_t>(cfg.versioning_snapshot_interval));

        if (new_key_id) {
            (void)sgrn::datastore::crypto::getKeyStore()->activate(*new_key_id, std::to_string(t_file_id));
        }

        co_return RestoreResult{r[0]["version_id"].as<int64_t>(), r[0]["version_no"].as<int32_t>(), r[0]["created"].as<bool>()};
    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, ex.what());
    }
}

// Retention: drop the oldest versions, crypto-shred their keys, delete orphaned blobs.
Task<Result<int, BackendError>> trimVersions(StorageService& t_svc, int64_t t_file_id, int t_keep, std::optional<int> t_older_than_days) {
    try {
        auto db_res = t_svc.getDbClient();
        auto s3_res = t_svc.S3Client();

        CO_RETURN_ERROR_IF(db_res.hasError() || s3_res.hasError(), BackendError(BackendErrorKind::Runtime, "Storage unavailable"));

        const std::string bucket = currentStorageConfig().default_bucket;

        auto rows = co_await db_res.value()->execSqlCoro(
            "SELECT version_id, key_id, blob_keys FROM storage.trim_versions($1, $2, $3::interval)", t_file_id, t_keep,
            t_older_than_days ? std::optional<std::string>(std::to_string(*t_older_than_days) + " days") : std::nullopt);

        std::vector<std::string> blobs;

        auto ks = sgrn::datastore::crypto::getKeyStore();

        for (const auto& r : rows) {
            if (!r["key_id"].isNull() && ks) {
                (void)ks->shred(r["key_id"].as<std::string>());
            }
            // text[] arrives as "{a,b}" — keys are generated by us and contain no commas/braces.
            std::string arr = r["blob_keys"].as<std::string>();
            if (arr.size() > 2) {
                arr = arr.substr(1, arr.size() - 2);
                size_t pos = 0;
                while (pos <= arr.size()) {
                    auto next = arr.find(',', pos);
                    blobs.push_back(arr.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
                    if (next == std::string::npos) {
                        break;
                    }
                    pos = next + 1;
                }
            }
        }

        co_await deleteBlobsIfRowGone(db_res.value(), s3_res.value(), bucket, blobs);

        co_return static_cast<int>(rows.size());
    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, ex.what());
    }
}

Task<Result<void, BackendError>> commitFileVersion(DbClientPtr t_db, int64_t t_file_id, const std::string& t_content_sha512,
    int64_t t_original_size, bool t_is_encrypted, std::optional<std::string> t_key_id, std::optional<int32_t> t_user_id,

    std::optional<int32_t> t_service_id, uint32_t t_snapshot_interval) {

    try {
        auto rows = co_await t_db->execSqlCoro("SELECT version_id FROM storage.commit_new_version($1,$2,$3,$4,$5,false,$6,$7,NULL,NULL,$8)",
            t_file_id, t_content_sha512, t_original_size, t_is_encrypted, t_key_id, t_user_id, t_service_id,
            static_cast<int32_t>(t_snapshot_interval));

        CO_RETURN_IF(rows.empty(), BackendError(BackendErrorKind::Database, "Version registration returned no version"));

        co_return {};

    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, std::string("Version registration failed: ") + ex.what());
    }
}
} // namespace sgrn::datastore::services::storage

// ============================================================================
