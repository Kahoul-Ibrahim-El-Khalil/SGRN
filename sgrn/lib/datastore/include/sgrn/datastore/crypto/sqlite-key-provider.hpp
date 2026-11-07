#pragma once
// sgrn/lib/datastore/include/sgrn/datastore/crypto/sqlite-key-provider.hpp
//
// SqliteKeyProvider: the ONLY module that touches $SGRN_DATA_DIR/keys/keys.db.
// Handlers and services never see key bytes beyond a short-lived Dek object
// that zeroes itself on destruction.
//
// Security requirements:
//   - Database path: $SGRN_DATA_DIR/keys/keys.db, mode 0600, dir mode 0700.
//   - Refuses startup if permissions are looser or the file is under the
//     Garage or Postgres data directories.
//   - PRAGMAs: journal_mode=WAL, synchronous=FULL, secure_delete=ON,
//              foreign_keys=ON, busy_timeout=5000.
//   - Reading a DEK goes through one function that checks wrap_scheme and
//     fails closed on any scheme it does not recognise.
//   - Errors must NEVER contain key bytes or plaintext data.
//
// SQLite schema (STRICT):
//   create table file_keys (
//     key_id      text primary key,
//     file_id     integer unique,          -- storage.files.id (set on activate)
//     object_key  text not null,           -- bucket/key of the ciphertext
//     dek         blob not null,           -- 32 raw bytes; wrapped blob later
//     wrap_scheme text not null default 'none',
//     kek_ref     text,
//     algorithm   text not null default 'AES-256-GCM',
//     chunk_size  integer not null,
//     state       text not null default 'pending'
//                     check (state in ('pending','active','shredded')),
//     created_at  integer not null default (unixepoch())
//   ) strict;

#include <sgrn/Result.hpp>
#include <sgrn/utils/crypto/key-provider.hpp>
#include <memory>
#include <string>

struct sqlite3;

namespace sgrn::datastore::crypto
{

/**
 * @brief Thread-safe, file-backed key provider using a local SQLite key store.
 *
 * Opening the database performs all security checks (permissions, path
 * isolation). Any subsequent failure that cannot be traced to a known
 * wrap_scheme is treated as a closed failure (error returned, no key exposed).
 */
class SqliteKeyProvider final : public ::sgrn::utils::crypto::KeyProvider {
public:
    /**
     * @brief Open (and if necessary create) the key database.
     *
     * @param t_db_path  Absolute path to keys.db. Must be under SGRN_DATA_DIR
     *                   and outside the Garage / Postgres data directories.
     * @param t_chunk_size_bytes  Default AEAD chunk size for newly created keys.
     *                            Stored in the row; used by the encrypt pipeline.
     */
    static ::sgrn::Result<std::unique_ptr<SqliteKeyProvider>> open(const std::string& t_db_path, uint32_t t_chunk_size_bytes = 1024 * 1024);

    ~SqliteKeyProvider() override;

    // Non-copyable, non-movable (holds a raw sqlite3* handle).
    SqliteKeyProvider(const SqliteKeyProvider&) = delete;
    SqliteKeyProvider& operator=(const SqliteKeyProvider&) = delete;
    SqliteKeyProvider(SqliteKeyProvider&&) = delete;
    SqliteKeyProvider& operator=(SqliteKeyProvider&&) = delete;

    // ── KeyProvider interface ────────────────────────────────────────────────

    /**
     * @brief Generate a fresh DEK, insert a 'pending' row, and fsync.
     *
     * The row is committed and fsynced BEFORE any ciphertext is uploaded.
     * A crash after this call leaves a pending key; the upload-cleanup service
     * will shred it after the upload TTL expires.
     *
     * @param t_object_key  Garage bucket/key where the ciphertext will be stored.
     */
    ::sgrn::Result<::sgrn::utils::crypto::KeyRecord> createPending(
        const std::string& t_object_key = "", const std::string& t_wrap_scheme = "none") override;

    /**
     * @brief Bind a pending key to a confirmed storage.files.id and mark ACTIVE.
     */
    ::sgrn::Result<void> activate(const std::string& t_key_id, const std::string& t_file_id) override;

    /**
     * @brief Retrieve an active key by key_id.
     * Returns error if the key is not found, shredded, or uses an unknown wrap_scheme.
     */
    ::sgrn::Result<::sgrn::utils::crypto::KeyRecord> get(const std::string& t_key_id) override;

    /**
     * @brief Crypto-shred: zeroise the dek blob in-place (via UPDATE with
     * zeroed blob + secure_delete PRAGMA) and set state='shredded'.
     * After shredding, the row remains for audit / reconciliation but the DEK
     * is irrecoverably gone.
     */
    ::sgrn::Result<void> shred(const std::string& t_key_id) override;

    /**
     * @brief List all key records for reconciliation.
     * DEK bytes are NOT returned (shredded rows have zero bytes anyway).
     * Used by --verify-keys CLI.
     */
    ::sgrn::Result<std::vector<::sgrn::utils::crypto::KeyRecord>> listForReconcile() override;

    /**
     * @brief Return the per-key chunk size stored in the row.
     * Used by the decrypt pipeline to reconstruct the stream layout.
     */
    ::sgrn::Result<uint32_t> getChunkSize(const std::string& t_key_id);

private:
    explicit SqliteKeyProvider(sqlite3* t_db, uint32_t t_default_chunk_size);

    ::sgrn::Result<void> applyPragmas();
    ::sgrn::Result<void> ensureSchema();

    sqlite3* m_db{nullptr};
    uint32_t m_default_chunk_size;
};

// ── Permission checks (called from open() and bootstrap verify) ──────────────

/**
 * @brief Verify that db_path has mode 0600 and its directory has mode 0700.
 * Returns an error string if the check fails.
 */
::sgrn::Result<void> verifyKeyDbPermissions(const std::string& t_db_path);

/**
 * @brief Verify that db_path is not under any of the exclusion directories
 * (Garage data_dir, Postgres data_dir).
 */
::sgrn::Result<void> verifyKeyDbIsolation(
    const std::string& t_db_path, const std::string& t_garage_data_dir, const std::string& t_postgres_data_dir);

} // namespace sgrn::datastore::crypto
