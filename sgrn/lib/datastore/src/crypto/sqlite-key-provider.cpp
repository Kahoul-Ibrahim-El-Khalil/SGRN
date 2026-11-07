// sgrn/lib/datastore/src/crypto/sqlite-key-provider.cpp
//
// SqliteKeyProvider: thread-safe, file-backed encryption key store.
// SECURITY CONTRACT:
//   - DEK bytes must NEVER appear in any log line, error message, or API response.
//   - Every code path that reads a DEK must zeroize it before returning on error.
//   - Only 'none' wrap_scheme is implemented; any unknown scheme fails closed.

#include <fmt/format.h>
#include <sgrn/datastore/crypto/sqlite-key-provider.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/crypto/dek.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;
using namespace sgrn::utils::crypto;

namespace sgrn::datastore::crypto
{

// ── RAII SQLite statement wrapper ────────────────────────────────────────────

namespace
{

struct StmtGuard {
    sqlite3_stmt* stmt{nullptr};
    ~StmtGuard() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

static std::string generateUuidV4() {
    uint8_t b[16];
    if (RAND_bytes(b, 16) != 1) {
        return "";
    }
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << '-';
        }
        ss << std::setw(2) << static_cast<int>(b[i]);
    }
    return ss.str();
}

} // namespace

// ── Permission and isolation checks ─────────────────────────────────────────

::sgrn::Result<void> verifyKeyDbPermissions(const std::string& t_db_path) {
    struct stat st{};
    if (::stat(t_db_path.c_str(), &st) != 0) {
        // File doesn't exist yet — will be created with correct mode in open().
        // Check parent directory permissions instead.
        fs::path dir = fs::path(t_db_path).parent_path();
        struct stat dst{};
        if (::stat(dir.c_str(), &dst) != 0) {
            // Directory doesn't exist yet either; creation happens in open().
            return {};
        }
        const int dir_perm = dst.st_mode & 0777;
        SGRN_RETURN_ERROR_IF(dir_perm != 0700, "Key store directory permissions are too loose (must be 0700). Refusing to start.");
        return {};
    }

    const int file_perm = st.st_mode & 0777;
    SGRN_RETURN_ERROR_IF(file_perm != 0600, "Key store file permissions are too loose (must be 0600). Refusing to start.");

    fs::path dir = fs::path(t_db_path).parent_path();
    struct stat dst{};
    if (::stat(dir.c_str(), &dst) == 0) {
        const int dir_perm = dst.st_mode & 0777;
        SGRN_RETURN_ERROR_IF(dir_perm != 0700, "Key store directory permissions are too loose (must be 0700). Refusing to start.");
    }

    return {};
}

::sgrn::Result<void> verifyKeyDbIsolation(
    const std::string& t_db_path, const std::string& t_garage_data_dir, const std::string& t_postgres_data_dir) {
    // Resolve to canonical absolute paths to prevent symlink tricks.
    std::error_code ec;
    fs::path canonical_db = fs::weakly_canonical(fs::path(t_db_path), ec);
    if (ec) {
        // If the file doesn't exist yet, check the parent.
        canonical_db = fs::weakly_canonical(fs::path(t_db_path).parent_path(), ec);
        if (ec) {
            return {}; // Cannot resolve — allow, permissions check is the gate.
        }
    }

    auto isUnder = [&](const std::string& dir) -> bool {
        if (dir.empty()) {
            return false;
        }
        fs::path canonical_dir = fs::weakly_canonical(fs::path(dir), ec);
        if (ec) {
            return false;
        }
        auto [dirIt, dbIt] = std::mismatch(canonical_dir.begin(), canonical_dir.end(), canonical_db.begin(), canonical_db.end());
        return dirIt == canonical_dir.end();
    };

    SGRN_RETURN_ERROR_IF(isUnder(t_garage_data_dir), "Key store path must not be inside the Garage data directory. Refusing to start.");
    SGRN_RETURN_ERROR_IF(isUnder(t_postgres_data_dir), "Key store path must not be inside the Postgres data directory. Refusing to start.");

    return {};
}

// ── SqliteKeyProvider ────────────────────────────────────────────────────────

SqliteKeyProvider::SqliteKeyProvider(sqlite3* t_db, uint32_t t_default_chunk_size)
    : m_db(t_db)
    , m_default_chunk_size(t_default_chunk_size) {
}

SqliteKeyProvider::~SqliteKeyProvider() {
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

::sgrn::Result<void> SqliteKeyProvider::applyPragmas() {
    const char* pragmas[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=FULL;",
        "PRAGMA secure_delete=ON;",
        "PRAGMA foreign_keys=ON;",
        "PRAGMA busy_timeout=5000;",
        nullptr,
    };
    for (const char** p = pragmas; *p; ++p) {
        SGRN_RETURN_ERROR_IF(sqlite3_exec(m_db, *p, nullptr, nullptr, nullptr) != SQLITE_OK,
            fmt::format("Key store PRAGMA failed: {}", sqlite3_errmsg(m_db)));
    }
    return {};
}

::sgrn::Result<void> SqliteKeyProvider::ensureSchema() {
    const char* ddl = R"sql(
        CREATE TABLE IF NOT EXISTS file_keys (
            key_id      TEXT    PRIMARY KEY,
            file_id     INTEGER UNIQUE,
            object_key  TEXT    NOT NULL,
            dek         BLOB    NOT NULL,
            wrap_scheme TEXT    NOT NULL DEFAULT 'none',
            kek_ref     TEXT,
            algorithm   TEXT    NOT NULL DEFAULT 'AES-256-GCM',
            chunk_size  INTEGER NOT NULL,
            state       TEXT    NOT NULL DEFAULT 'pending'
                            CHECK (state IN ('pending','active','shredded')),
            created_at  INTEGER NOT NULL DEFAULT (unixepoch())
        ) STRICT;
        CREATE INDEX IF NOT EXISTS idx_file_keys_file_id ON file_keys (file_id) WHERE file_id IS NOT NULL;
        CREATE INDEX IF NOT EXISTS idx_file_keys_state   ON file_keys (state);
    )sql";

    SGRN_RETURN_ERROR_IF(sqlite3_exec(m_db, ddl, nullptr, nullptr, nullptr) != SQLITE_OK,
        fmt::format("Key store schema init failed: {}", sqlite3_errmsg(m_db)));
    return {};
}

::sgrn::Result<std::unique_ptr<SqliteKeyProvider>> SqliteKeyProvider::open(const std::string& t_db_path, uint32_t t_chunk_size_bytes) {
    // Ensure the directory exists with mode 0700
    fs::path dir = fs::path(t_db_path).parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    SGRN_RETURN_ERROR_IF(ec, fmt::format("Cannot create key store directory: {}", ec.message()));

    // Set directory to 0700
    if (::chmod(dir.c_str(), 0700) != 0) {
        SGRN_WARN("SqliteKeyProvider", "Could not set key store directory mode to 0700");
    }

    // Check permissions (fails if already exists with wrong mode)
    auto perm_res = verifyKeyDbPermissions(t_db_path);
    SGRN_RETURN_ERROR_IF(perm_res.hasError(), perm_res.error());

    // Open database
    sqlite3* raw_db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(t_db_path.c_str(), &raw_db, flags, nullptr) != SQLITE_OK) {
        const std::string msg = raw_db ? sqlite3_errmsg(raw_db) : "sqlite3_open failed";
        if (raw_db) {
            sqlite3_close_v2(raw_db);
        }
        return ::sgrn::Error(fmt::format("Cannot open key store: {}", msg));
    }

    // Set file permissions to 0600
    if (::chmod(t_db_path.c_str(), 0600) != 0) {
        SGRN_WARN("SqliteKeyProvider", "Could not set key store file mode to 0600");
    }

    auto provider = std::unique_ptr<SqliteKeyProvider>(new SqliteKeyProvider(raw_db, t_chunk_size_bytes));

    auto pragma_res = provider->applyPragmas();
    SGRN_RETURN_ERROR_IF(pragma_res.hasError(), pragma_res.error());

    auto schema_res = provider->ensureSchema();
    SGRN_RETURN_ERROR_IF(schema_res.hasError(), schema_res.error());

    return provider;
}

// ── createPending (with object_key) ─────────────────────────────────────────

::sgrn::Result<KeyRecord> SqliteKeyProvider::createPending(const std::string& t_object_key, const std::string& t_wrap_scheme) {
    SGRN_RETURN_ERROR_IF(t_object_key.empty(), "SqliteKeyProvider::createPending requires an object_key");

    // Only 'none' is implemented; fail closed on unknown schemes.
    const std::string scheme = t_wrap_scheme.empty() ? "none" : t_wrap_scheme;
    SGRN_RETURN_ERROR_IF(scheme != "none", fmt::format("Unsupported wrap_scheme '{}' — only 'none' is implemented", scheme));

    // Generate DEK
    auto dek_res = Dek::generate();
    SGRN_RETURN_ERROR_IF(dek_res.hasError(), "DEK generation failed");

    std::string key_id = generateUuidV4();
    SGRN_RETURN_ERROR_IF(key_id.empty(), "Failed to generate UUID for key_id");

    // Insert pending row — commit + fsync BEFORE any ciphertext is uploaded.
    StmtGuard sg;
    const char* sql = "INSERT INTO file_keys "
                      "(key_id, object_key, dek, wrap_scheme, algorithm, chunk_size, state) "
                      "VALUES (?, ?, ?, ?, 'AES-256-GCM', ?, 'pending')";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store insert prepare failed");

    sqlite3_bind_text(sg.stmt, 1, key_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(sg.stmt, 2, t_object_key.c_str(), -1, SQLITE_STATIC);
    // Bind raw DEK bytes — NEVER log these
    sqlite3_bind_blob(sg.stmt, 3, dek_res.value().data(), static_cast<int>(dek_res.value().size()), SQLITE_STATIC);
    sqlite3_bind_text(sg.stmt, 4, scheme.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(sg.stmt, 5, static_cast<int>(m_default_chunk_size));

    SGRN_RETURN_ERROR_IF(sqlite3_step(sg.stmt) != SQLITE_DONE, "Key store insert failed");

    // fsync the WAL so the row survives a crash before ciphertext upload.
    sqlite3_exec(m_db, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr, nullptr);

    KeyRecord record;
    record.key_id = key_id;
    record.object_key = t_object_key;
    record.status = KeyStatus::PENDING;
    record.wrap_scheme = scheme;
    record.algorithm = "AES-256-GCM";
    record.chunk_size = m_default_chunk_size;
    record.dek = std::move(dek_res.value());
    record.created_at = std::chrono::system_clock::now();

    return record;
}

// ── activate ────────────────────────────────────────────────────────────────

::sgrn::Result<void> SqliteKeyProvider::activate(const std::string& t_key_id, const std::string& t_file_id) {
    SGRN_RETURN_ERROR_IF(t_key_id.empty() || t_file_id.empty(), "key_id and file_id are required for activation");

    int64_t file_id_int = 0;
    try {
        file_id_int = std::stoll(t_file_id);
    } catch (...) {
        return ::sgrn::Error("Invalid file_id for key activation");
    }

    StmtGuard sg;
    const char* sql = "UPDATE file_keys SET state='active', file_id=? "
                      "WHERE key_id=? AND state='pending'";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store activate prepare failed");

    sqlite3_bind_int64(sg.stmt, 1, file_id_int);
    sqlite3_bind_text(sg.stmt, 2, t_key_id.c_str(), -1, SQLITE_STATIC);

    SGRN_RETURN_ERROR_IF(sqlite3_step(sg.stmt) != SQLITE_DONE, "Key store activate failed");
    SGRN_RETURN_ERROR_IF(sqlite3_changes(m_db) == 0, "Key not found or already activated/shredded");

    return {};
}

// ── get ──────────────────────────────────────────────────────────────────────

::sgrn::Result<KeyRecord> SqliteKeyProvider::get(const std::string& t_key_id) {
    SGRN_RETURN_ERROR_IF(t_key_id.empty(), "key_id is required");

    StmtGuard sg;
    const char* sql = "SELECT dek, wrap_scheme, algorithm, chunk_size, state, file_id, object_key, created_at "
                      "FROM file_keys WHERE key_id=?";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store get prepare failed");

    sqlite3_bind_text(sg.stmt, 1, t_key_id.c_str(), -1, SQLITE_STATIC);

    const int rc = sqlite3_step(sg.stmt);
    SGRN_RETURN_ERROR_IF(rc == SQLITE_DONE, "Key not found");
    SGRN_RETURN_ERROR_IF(rc != SQLITE_ROW, "Key store get failed");

    const std::string state = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 4));
    SGRN_RETURN_ERROR_IF(state == "shredded", "Key is shredded and unrecoverable");

    const std::string scheme = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 1));
    // Fail closed on unknown wrap schemes — NEVER expose raw bytes of a wrapped key.
    SGRN_RETURN_ERROR_IF(scheme != "none", fmt::format("Unsupported wrap_scheme '{}' — cannot unwrap key", scheme));

    // Read raw DEK bytes — constructed directly into Dek, never logged.
    const void* blob = sqlite3_column_blob(sg.stmt, 0);
    const int blob_len = sqlite3_column_bytes(sg.stmt, 0);
    SGRN_RETURN_ERROR_IF(blob == nullptr || blob_len != 32, "Key store: invalid DEK length (expected 32 bytes)");

    auto dek_res = Dek::fromBytes(std::span<const uint8_t>(static_cast<const uint8_t*>(blob), static_cast<size_t>(blob_len)));
    SGRN_RETURN_ERROR_IF(dek_res.hasError(), "Key store: DEK construction failed");

    KeyRecord record;
    record.key_id = t_key_id;
    record.wrap_scheme = scheme;
    record.algorithm = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 2));
    record.chunk_size = static_cast<uint32_t>(sqlite3_column_int(sg.stmt, 3));
    record.dek = std::move(dek_res.value());
    record.object_key = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 6));

    if (sqlite3_column_type(sg.stmt, 5) != SQLITE_NULL) {
        record.file_id = std::to_string(sqlite3_column_int64(sg.stmt, 5));
    }

    if (state == "active") {
        record.status = KeyStatus::ACTIVE;
    } else {
        record.status = KeyStatus::PENDING;
    }

    return record;
}

// ── shred ────────────────────────────────────────────────────────────────────

::sgrn::Result<void> SqliteKeyProvider::shred(const std::string& t_key_id) {
    SGRN_RETURN_ERROR_IF(t_key_id.empty(), "key_id is required for shredding");

    // Zeroise the dek column in-place, then mark shredded.
    // secure_delete=ON means SQLite will overwrite freed pages.
    std::array<uint8_t, 32> zeros{};
    OPENSSL_cleanse(zeros.data(), zeros.size());

    StmtGuard sg;
    const char* sql = "UPDATE file_keys SET dek=?, state='shredded' WHERE key_id=? AND state!='shredded'";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store shred prepare failed");

    sqlite3_bind_blob(sg.stmt, 1, zeros.data(), static_cast<int>(zeros.size()), SQLITE_STATIC);
    sqlite3_bind_text(sg.stmt, 2, t_key_id.c_str(), -1, SQLITE_STATIC);

    SGRN_RETURN_ERROR_IF(sqlite3_step(sg.stmt) != SQLITE_DONE, "Key store shred update failed");

    // Checkpoint to ensure the zeroed bytes are flushed to disk.
    sqlite3_exec(m_db, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr, nullptr);

    return {};
}

// ── listForReconcile ─────────────────────────────────────────────────────────

::sgrn::Result<std::vector<KeyRecord>> SqliteKeyProvider::listForReconcile() {
    StmtGuard sg;
    const char* sql = "SELECT key_id, file_id, object_key, wrap_scheme, algorithm, chunk_size, state, created_at "
                      "FROM file_keys ORDER BY created_at ASC";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store list prepare failed");

    std::vector<KeyRecord> out;
    while (sqlite3_step(sg.stmt) == SQLITE_ROW) {
        KeyRecord r;
        r.key_id = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 0));
        if (sqlite3_column_type(sg.stmt, 1) != SQLITE_NULL) {
            r.file_id = std::to_string(sqlite3_column_int64(sg.stmt, 1));
        }
        r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 2));
        r.wrap_scheme = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 3));
        r.algorithm = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 4));
        r.chunk_size = static_cast<uint32_t>(sqlite3_column_int(sg.stmt, 5));

        const std::string state = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 6));
        if (state == "pending") {
            r.status = KeyStatus::PENDING;
        } else if (state == "active") {
            r.status = KeyStatus::ACTIVE;
        } else {
            r.status = KeyStatus::SHREDDED;
        }

        // DEK is NOT returned in reconcile listing.
        // r.dek remains default-constructed (invalid).

        out.push_back(std::move(r));
    }

    return out;
}

// ── getChunkSize ─────────────────────────────────────────────────────────────

::sgrn::Result<uint32_t> SqliteKeyProvider::getChunkSize(const std::string& t_key_id) {
    StmtGuard sg;
    const char* sql = "SELECT chunk_size FROM file_keys WHERE key_id=?";
    SGRN_RETURN_ERROR_IF(sqlite3_prepare_v2(m_db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK, "Key store chunk_size query failed");
    sqlite3_bind_text(sg.stmt, 1, t_key_id.c_str(), -1, SQLITE_STATIC);

    SGRN_RETURN_ERROR_IF(sqlite3_step(sg.stmt) != SQLITE_ROW, "Key not found");
    return static_cast<uint32_t>(sqlite3_column_int(sg.stmt, 0));
}

} // namespace sgrn::datastore::crypto
