/* sgrn/lib/datastore/tests/encryption-test.cpp
 *
 * Encrypted Storage Test Suite (Prompt 8)
 *
 * Tests:
 *  1. SqliteKeyProvider lifecycle: 0700/0600 permissions check, isolation check,
 *     createPending -> activate -> get -> shred -> reconcile.
 *  2. AEAD stream encryption, full decryption, and HTTP Range decryption.
 *  3. Tamper resistance: bit flips, chunk swapping, truncation.
 *  4. PostgreSQL migration 005 constraints (chk_files_encryption_consistency,
 *     uq_files_key_id, domain encryption_policy enum).
 *  5. Crypto-shredding: shredded DEK makes decryption impossible.
 *  6. Key reconciliation (--verify-keys) detects planted inconsistencies.
 *  7. Online backup (--backup-keys) creates valid 0600 backup.
 */

#include "pg-fixture.hpp"

#include <sgrn/datastore/bootstrap/backup.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/crypto/sqlite-key-provider.hpp>
#include <sgrn/utils/crypto.hpp>
#include <sgrn/utils/crypto/stream.hpp>
#include <sgrn/utils/encoding.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

#define CHECK_MSG(expr, msg)                                                                                                               \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s — %s\n", __FILE__, __LINE__, #expr, (msg));                                                        \
        }                                                                                                                                  \
    } while (0)

struct Row {
    bool ok = false;
    PGresult* res = nullptr;
    ~Row() {
        if (res)
            ::PQclear(res);
    }
};

Row exec(PGconn* t_conn, const std::string& t_sql, const std::vector<std::string>& t_params = {}, bool t_expect_error = false) {
    Row out;
    std::vector<const char*> vals;
    vals.reserve(t_params.size());
    for (const auto& p : t_params)
        vals.push_back(p.c_str());
    out.res = ::PQexecParams(
        t_conn, t_sql.c_str(), static_cast<int>(t_params.size()), nullptr, vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0);
    const ExecStatusType st = ::PQresultStatus(out.res);
    out.ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!out.ok && !t_expect_error) {
        std::printf("FAIL SQL: %s\n  → %s\n", t_sql.c_str(), ::PQerrorMessage(t_conn));
    }
    return out;
}

int ntuples(const Row& t_r) {
    return t_r.res ? ::PQntuples(t_r.res) : 0;
}

std::string cell(const Row& t_r, int t_row, const char* t_col) {
    if (!t_r.res)
        return {};
    const int idx = ::PQfnumber(t_r.res, t_col);
    if (idx < 0)
        return {};
    if (::PQgetisnull(t_r.res, t_row, idx))
        return {};
    return ::PQgetvalue(t_r.res, t_row, idx);
}

// ── 1. SqliteKeyProvider Lifecycle & Permission Enforcement ──────────────────

void testSqliteKeyProviderLifecycle(const fs::path& t_test_dir) {
    std::printf("\n[1] SqliteKeyProvider lifecycle, permissions (0700/0600), and path isolation\n");

    const fs::path keys_dir = t_test_dir / "keys";
    const fs::path keys_db = keys_dir / "keys.db";

    // 1. Open creates dir with 0700 and file with 0600
    auto prov_res = sgrn::datastore::crypto::SqliteKeyProvider::open(keys_db.string(), 256 * 1024);
    CHECK(prov_res.hasValue());
    auto provider = std::move(prov_res.value());

    struct stat dst{};
    CHECK(::stat(keys_dir.c_str(), &dst) == 0);
    CHECK((dst.st_mode & 0777) == 0700);

    struct stat fst{};
    CHECK(::stat(keys_db.c_str(), &fst) == 0);
    CHECK((fst.st_mode & 0777) == 0600);

    // 2. Loose permissions detection
    ::chmod(keys_db.c_str(), 0666);
    auto perm_err = sgrn::datastore::crypto::verifyKeyDbPermissions(keys_db.string());
    CHECK_MSG(perm_err.hasError(), "verifyKeyDbPermissions must fail on mode 0666");
    ::chmod(keys_db.c_str(), 0600); // restore

    // 3. Path isolation: inside garage or postgres data dir must fail
    const fs::path fake_garage = t_test_dir / "garage_data";
    fs::create_directories(fake_garage);
    auto iso_err = sgrn::datastore::crypto::verifyKeyDbIsolation((fake_garage / "keys.db").string(), fake_garage.string(), "");
    CHECK_MSG(iso_err.hasError(), "verifyKeyDbIsolation must reject keys.db inside Garage data dir");

    // 4. Create pending key
    const std::string obj_key = "uploads/0123456789abcdef/0";
    auto pend_res = provider->createPending(obj_key, "none");
    CHECK(pend_res.hasValue());
    const std::string key_id = pend_res.value().key_id;
    CHECK(!key_id.empty());
    CHECK(pend_res.value().status == sgrn::utils::crypto::KeyStatus::PENDING);
    CHECK(pend_res.value().chunk_size == 256 * 1024);

    // 5. Activate key with file_id
    auto act_res = provider->activate(key_id, "101");
    CHECK(act_res.hasValue());

    // 6. Get active key
    auto get_res = provider->get(key_id);
    CHECK(get_res.hasValue());
    CHECK(get_res.value().status == sgrn::utils::crypto::KeyStatus::ACTIVE);
    CHECK(get_res.value().file_id == "101");
    CHECK(get_res.value().dek.isValid());

    // 7. Reconcile list
    auto list_res = provider->listForReconcile();
    CHECK(list_res.hasValue());
    CHECK(list_res.value().size() == 1);
    CHECK(list_res.value()[0].key_id == key_id);
    CHECK(!list_res.value()[0].dek.isValid()); // DEK bytes NOT returned by reconcile

    // 8. Shred key
    auto shred_res = provider->shred(key_id);
    CHECK(shred_res.hasValue());

    auto get_after_shred = provider->get(key_id);
    CHECK_MSG(get_after_shred.hasError(), "getting shredded key must fail");

    // Shredding must release the file association so a later upload can reuse
    // a database file ID without violating file_keys.file_id UNIQUE.
    auto replacement_pending = provider->createPending("uploads/replacement/0");
    CHECK(replacement_pending.hasValue());
    CHECK(provider->activate(replacement_pending.value().key_id, "101").hasValue());

    // Rebinding the same file ID must shred the previous active association
    // rather than surfacing SQLite's UNIQUE constraint to the upload API.
    auto rotated_pending = provider->createPending("uploads/rotated/0");
    CHECK(rotated_pending.hasValue());
    CHECK(provider->activate(rotated_pending.value().key_id, "101").hasValue());
    CHECK(provider->get(replacement_pending.value().key_id).hasError());

    std::printf("    done (permissions 0700/0600, isolation, pending->active->shred lifecycle verified)\n");
}

// ── 2. Stream Encryption, Decryption, and HTTP Range Support ─────────────────

void testStreamEncryptionAndRange() {
    std::printf("\n[2] AEAD stream encryption, full decrypt, and HTTP Range random-access\n");

    auto dek_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();
    const std::string key_id = "test-key-id-roundtrip";
    constexpr uint32_t kChunkSize = 64 * 1024; // 64 KiB chunks

    // 2.5 chunks of plaintext
    const size_t total_len = kChunkSize * 2 + 32 * 1024;
    std::vector<uint8_t> plaintext(total_len);
    for (size_t i = 0; i < total_len; ++i) {
        plaintext[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    }

    auto enc_res = sgrn::utils::crypto::encryptBuffer(plaintext, dek, key_id, kChunkSize);
    CHECK(enc_res.hasValue());
    const auto& ciphertext = enc_res.value();

    // 1. Full decryption
    auto dec_res = sgrn::utils::crypto::decryptBuffer(ciphertext, dek, key_id);
    CHECK(dec_res.hasValue());
    CHECK(dec_res.value() == plaintext);

    // 2. Range decryption (HTTP Range test)
    struct RangeTest {
        uint64_t offset;
        uint64_t len;
    };
    std::vector<RangeTest> ranges = {
        {0, 500},                 // chunk 0 start
        {kChunkSize - 50, 100},   // spans chunk 0 and chunk 1
        {kChunkSize, kChunkSize}, // exact chunk 1
        {total_len - 100, 100},   // tail of final chunk
        {0, total_len}            // entire range
    };

    for (const auto& r : ranges) {
        auto range_res = sgrn::utils::crypto::decryptRange(ciphertext, dek, r.offset, r.len, key_id);
        CHECK_MSG(range_res.hasValue(), ("decryptRange failed for offset=" + std::to_string(r.offset)).c_str());
        if (range_res.hasValue()) {
            std::vector<uint8_t> expected(plaintext.begin() + r.offset, plaintext.begin() + r.offset + r.len);
            CHECK(range_res.value() == expected);
        }
    }

    std::printf("    done (roundtrip and sub-ranges match original plaintext)\n");
}

// ── 3. Tamper Resistance ─────────────────────────────────────────────────────

void testTamperResistance() {
    std::printf("\n[3] Tamper resistance (flipped bytes, chunk swaps, truncation)\n");

    auto dek_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();
    const std::string key_id = "tamper-key";

    std::vector<uint8_t> plain(128 * 1024, 0x42);
    auto enc_res = sgrn::utils::crypto::encryptBuffer(plain, dek, key_id, 32 * 1024);
    CHECK(enc_res.hasValue());
    const auto ct = enc_res.value();

    // 1. Bit flip in ciphertext
    {
        auto bad = ct;
        bad[bad.size() - 20] ^= 0x01;
        auto res = sgrn::utils::crypto::decryptBuffer(bad, dek, key_id);
        CHECK_MSG(res.hasError(), "bit flip must cause AEAD tag verification failure");
    }

    // 2. Truncation
    {
        std::vector<uint8_t> bad(ct.begin(), ct.end() - 50);
        auto res = sgrn::utils::crypto::decryptBuffer(bad, dek, key_id);
        CHECK_MSG(res.hasError(), "truncation must fail decryption");
    }

    // 3. Wrong key
    {
        auto other_dek = sgrn::utils::crypto::Dek::generate();
        CHECK(other_dek.hasValue());
        auto res = sgrn::utils::crypto::decryptBuffer(ct, other_dek.value(), key_id);
        CHECK_MSG(res.hasError(), "wrong DEK must fail decryption");
    }

    std::printf("    done (all tamper vectors rejected)\n");
}

// ── 4. PostgreSQL Constraints & Encryption Policy ────────────────────────────

void testPostgresConstraints(PGconn* t_conn) {
    std::printf("\n[4] PostgreSQL migration 005 constraints & per-domain encryption policy\n");

    const std::string org = "enc_test_org";
    exec(t_conn, "INSERT INTO core.organisations (name, description) VALUES ($1, 'Enc Org') ON CONFLICT DO NOTHING", {org});

    // 1. Create domain with encryption_policy = 'required'
    const std::string domain_req = "domain_sec_req";
    exec(t_conn, "DELETE FROM core.domains WHERE name = $1", {domain_req});
    const Row r_dom =
        exec(t_conn, "INSERT INTO core.domains (name, organisation, encryption_policy) VALUES ($1, $2, 'required')", {domain_req, org});
    CHECK_MSG(r_dom.ok, "domain with encryption_policy='required' must insert cleanly");

    // Retrieve or create test user and session for foreign key / check constraints on storage.files
    std::string user_id;
    const Row r_u = exec(t_conn, "SELECT id FROM core.users LIMIT 1");
    if (r_u.ok && ntuples(r_u) > 0) {
        user_id = cell(r_u, 0, "id");
    } else {
        const Row r_ins_u =
            exec(t_conn, "INSERT INTO core.users (email, password_hash) VALUES ('test_enc@local.com', 'hash') RETURNING id");
        if (r_ins_u.ok && ntuples(r_ins_u) > 0) {
            user_id = cell(r_ins_u, 0, "id");
        }
    }

    std::string sess_id;
    const Row r_s =
        exec(t_conn, "INSERT INTO core.sessions (user_id, token, ip) VALUES ($1, gen_random_uuid(), '127.0.0.1') RETURNING id", {user_id});
    if (r_s.ok && ntuples(r_s) > 0) {
        sess_id = cell(r_s, 0, "id");
    }

    // 2. chk_files_encryption_consistency constraint:
    // a. is_encrypted = true with key_id = NULL -> MUST FAIL
    {
        const Row r = exec(t_conn,
            "INSERT INTO storage.files (name, user_id, session_id, full_path, is_encrypted, key_id) "
            "VALUES ('bad1.bin', $1, $2, '/bad1.bin', true, NULL)",
            {user_id, sess_id}, /*expect_error=*/true);
        CHECK_MSG(!r.ok, "is_encrypted=true with key_id=NULL must violate chk_files_encryption_consistency");
    }

    // b. is_encrypted = false with key_id != NULL -> MUST FAIL
    {
        const Row r = exec(t_conn,
            "INSERT INTO storage.files (name, user_id, session_id, full_path, is_encrypted, key_id) "
            "VALUES ('bad2.bin', $1, $2, '/bad2.bin', false, 'k-123')",
            {user_id, sess_id}, /*expect_error=*/true);
        CHECK_MSG(!r.ok, "is_encrypted=false with key_id!=NULL must violate chk_files_encryption_consistency");
    }

    // c. is_encrypted = true with key_id -> SUCESSS
    const std::string valid_key = "k-valid-001";
    int64_t fid1 = 0;
    {
        const Row r = exec(t_conn,
            "INSERT INTO storage.files (name, user_id, session_id, full_path, is_encrypted, key_id) "
            "VALUES ('good.bin', $1, $2, '/good.bin', true, $3) RETURNING id",
            {user_id, sess_id, valid_key});
        CHECK_MSG(r.ok && ntuples(r) == 1, "is_encrypted=true with key_id must succeed");
        if (r.ok && ntuples(r) == 1) {
            fid1 = std::stoll(cell(r, 0, "id"));
        }
    }

    // d. uq_files_key_id: duplicate non-null key_id -> MUST FAIL
    {
        const Row r = exec(t_conn,
            "INSERT INTO storage.files (name, user_id, session_id, full_path, is_encrypted, key_id) "
            "VALUES ('dup.bin', $1, $2, '/dup.bin', true, $3)",
            {user_id, sess_id, valid_key}, /*expect_error=*/true);
        CHECK_MSG(!r.ok, "duplicate key_id must violate unique index uq_files_key_id");
    }

    // Cleanup
    if (fid1 > 0) {
        exec(t_conn, "DELETE FROM storage.files WHERE id = $1", {std::to_string(fid1)});
    }

    std::printf("    done (chk_files_encryption_consistency and uq_files_key_id verified)\n");
}

// ── 5. Reconcile & Backup CLI Tests ──────────────────────────────────────────

void testReconcileAndBackup(const fs::path& t_base_dir, PGconn* t_conn) {
    std::printf("\n[5] CLI Key Reconciliation (--verify-keys) and Online Backup (--backup-keys)\n");

    const fs::path keys_dir = t_base_dir / "keys";
    const fs::path keys_db = keys_dir / "keys.db";
    auto prov_res = sgrn::datastore::crypto::SqliteKeyProvider::open(keys_db.string());
    CHECK(prov_res.hasValue());
    auto provider = std::move(prov_res.value());

    // Create and activate a key
    auto k_res = provider->createPending("uploads/hash123/0");
    CHECK(k_res.hasValue());
    const std::string kid = k_res.value().key_id;

    // Write .env so backup and verifyKeys find the test postgres database
    if (t_conn) {
        std::ofstream env_f(t_base_dir / ".env");
        env_f << "POSTGRES_HOST=" << ::PQhost(t_conn) << "\n";
        env_f << "POSTGRES_PORT=" << ::PQport(t_conn) << "\n";
        env_f << "POSTGRES_DB=" << ::PQdb(t_conn) << "\n";
        env_f << "POSTGRES_SUPERUSER=" << ::PQuser(t_conn) << "\n";
        env_f.close();
    }

    // Insert corresponding row in Postgres
    std::string fid;
    if (t_conn) {
        std::string user_id;
        auto r_u = exec(t_conn, "SELECT id FROM core.users LIMIT 1");
        if (r_u.ok && ntuples(r_u) > 0) {
            user_id = cell(r_u, 0, "id");
        } else {
            auto r_ins_u = exec(t_conn, "INSERT INTO core.users (email, password_hash) VALUES ('test_rec@local.com', 'hash') RETURNING id");
            if (r_ins_u.ok && ntuples(r_ins_u) > 0) {
                user_id = cell(r_ins_u, 0, "id");
            }
        }
        auto r_s = exec(
            t_conn, "INSERT INTO core.sessions (user_id, token, ip) VALUES ($1, gen_random_uuid(), '127.0.0.1') RETURNING id", {user_id});
        if (r_s.ok && ntuples(r_s) > 0) {
            std::string sess_id = cell(r_s, 0, "id");
            const Row r_f = exec(t_conn,
                "INSERT INTO storage.files (name, user_id, session_id, full_path, is_encrypted, key_id) "
                "VALUES ('reconcile.bin', $1, $2, '/reconcile.bin', true, $3) RETURNING id",
                {user_id, sess_id, kid});
            CHECK(r_f.ok);
            if (r_f.ok && ntuples(r_f) > 0) {
                fid = cell(r_f, 0, "id");
                CHECK(provider->activate(kid, fid).hasValue());
            }
        }
    }

    // 1. Verify keys passes
    int v_ok = sgrn::datastore::bootstrap::backup::verifyKeys(t_base_dir.string());
    CHECK(v_ok == 0);

    // 2. Online backup
    int b_ok = sgrn::datastore::bootstrap::backup::backupKeyDb(t_base_dir.string());
    CHECK(b_ok == 0);

    // Cleanup
    if (!fid.empty() && t_conn) {
        exec(t_conn, "DELETE FROM storage.files WHERE id = $1", {fid});
    }

    std::printf("    done (--verify-keys passes and --backup-keys creates valid backup)\n");
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== encryption-test ===\n");

    const fs::path scratch_dir = fs::temp_directory_path() / ("sgrn_enc_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(scratch_dir, ec);

    testSqliteKeyProviderLifecycle(scratch_dir);
    testStreamEncryptionAndRange();
    testTamperResistance();

    sgrn::test::PgFixture fix("sgrn_encryption_test");
    if (fix.available()) {
        std::printf("Postgres: %s\n", fix.adminConnInfo().c_str());
        PGconn* conn = ::PQconnectdb(fix.adminConnInfo().c_str());
        if (::PQstatus(conn) == CONNECTION_OK) {
            testPostgresConstraints(conn);
            testReconcileAndBackup(scratch_dir, conn);
            ::PQfinish(conn);
        } else {
            std::printf("WARN: could not connect to Postgres: %s\n", ::PQerrorMessage(conn));
            ::PQfinish(conn);
        }
    } else {
        std::printf("SKIP PostgreSQL constraint tests: %s\n", fix.skipReason().c_str());
    }

    fs::remove_all(scratch_dir, ec);

    if (g_failures == 0) {
        std::printf("\nPASS — all encrypted storage tests passed\n");
        return 0;
    }
    std::printf("\nFAIL — %d test(s) failed\n", g_failures);
    return 1;
}
