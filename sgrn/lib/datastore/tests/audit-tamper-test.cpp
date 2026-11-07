/* sgrn/lib/datastore/tests/audit-tamper-test.cpp
 *
 * Tamper-Evident Audit Log Test Suite (Prompt 6)
 *
 * Tests:
 *  1. Sequential append: insert 10 rows, verify seq 1..10, prev_hash chain, row_hash integrity.
 *  2. SQL tamper test: UPDATE row 5; verify chain fails at seq 5.
 *  3. SQL delete test: DELETE row 3; verify chain fails at seq 4 (gap/hash mismatch).
 *  4. Concurrent writers: 8 threads × 50 rows → 400-row unbroken monotonic chain.
 *  5. Ed25519 checkpoint: sign at seq 400, verify OK; flip byte → verify fails.
 *
 * Environment: same PGHOST / PGPORT / PGUSER / PGPASSWORD convention as the
 * other pg-fixture-backed tests. Missing Postgres → SKIP + exit 0.
 */

#include "pg-fixture.hpp"

#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/utils/crypto.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/keygen.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

// ── Helpers ──────────────────────────────────────────────────────────────────

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

Row exec(PGconn* t_conn, const std::string& t_sql, const std::vector<std::string>& t_params = {}) {
    Row out;
    std::vector<const char*> vals;
    vals.reserve(t_params.size());
    for (const auto& p : t_params)
        vals.push_back(p.c_str());
    out.res = ::PQexecParams(
        t_conn, t_sql.c_str(), static_cast<int>(t_params.size()), nullptr, vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0);
    if (!out.res) {
        std::printf("FAIL PQexecParams NULL: %s\n", ::PQerrorMessage(t_conn));
        return out;
    }
    const ExecStatusType st = ::PQresultStatus(out.res);
    out.ok = (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK);
    if (!out.ok)
        std::printf("FAIL SQL: %s\n  → %s\n", t_sql.substr(0, 120).c_str(), ::PQresultErrorMessage(out.res));
    return out;
}

std::string cell(const Row& t_r, int t_row, const char* t_col) {
    if (!t_r.res)
        return {};
    const int col = ::PQfnumber(t_r.res, t_col);
    if (col < 0 || t_row >= ::PQntuples(t_r.res))
        return {};
    const char* v = ::PQgetvalue(t_r.res, t_row, col);
    return v ? v : "";
}

int ntuples(const Row& t_r) {
    return t_r.res ? ::PQntuples(t_r.res) : -1;
}

/// Call core.append_audit_log via the fixture's admin connection.
bool appendLog(PGconn* t_conn, const std::string& t_org, const std::string& t_action, const std::string& t_target_id = "obj1") {
    const Row r = exec(t_conn,
        "SELECT core.append_audit_log("
        "$1::text, 'user', 1, 'tester@example.com', "
        "$2::text, 'audit_test', $3::text, '127.0.0.1', "
        "'{}'::jsonb, 'success')",
        {t_org, t_action, t_target_id});
    return r.ok;
}

/// Returns the full verify result row (is_valid, first_broken_seq, failure_reason).
Row verifyChain(PGconn* t_conn, const std::string& t_org) {
    return exec(t_conn,
        "SELECT is_valid, first_broken_seq, failure_reason "
        "FROM core.verify_audit_chain($1::text)",
        {t_org});
}

/// Delete all audit data for a scratch organisation and ensure the org exists.
void wipeOrg(PGconn* t_conn, const std::string& t_org) {
    // Ensure the organisation exists to satisfy audit_logs foreign key constraint
    exec(t_conn, "INSERT INTO core.organisations (name) VALUES ($1) ON CONFLICT (name) DO NOTHING", {t_org});
    // Temporarily disable the tamper guard so the fixture can reset state.
    exec(t_conn, "SET session_replication_role = replica");
    exec(t_conn, "DELETE FROM core.audit_logs WHERE organisation = $1", {t_org});
    exec(t_conn, "DELETE FROM core.audit_checkpoints WHERE organisation = $1", {t_org});
    exec(t_conn, "SET session_replication_role = DEFAULT");
}

// ── Test cases ───────────────────────────────────────────────────────────────

void testSequentialAppendAndVerify(PGconn* t_conn) {
    std::printf("\n[1] Sequential append and verify\n");
    const std::string org = "audit_test_seq";
    wipeOrg(t_conn, org);

    // Insert 10 rows
    for (int i = 1; i <= 10; ++i) {
        const bool ok = appendLog(t_conn, org, "test.action_" + std::to_string(i));
        CHECK_MSG(ok, ("append row " + std::to_string(i)).c_str());
    }

    // Check seq column is 1..10
    {
        const Row r = exec(t_conn,
            "SELECT seq FROM core.audit_logs WHERE organisation = $1 "
            "ORDER BY seq",
            {org});
        CHECK(r.ok);
        CHECK(ntuples(r) == 10);
        for (int i = 0; i < ntuples(r); ++i)
            CHECK(cell(r, i, "seq") == std::to_string(i + 1));
    }

    // Verify chain: must be valid
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok);
        CHECK(ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "is_valid") == "t", "chain must be valid after sequential appends");
        CHECK(cell(r, 0, "first_broken_seq").empty() || cell(r, 0, "first_broken_seq") == "");
    }

    // Spot-check: row 1 has prev_hash = '' (genesis)
    {
        const Row r = exec(t_conn,
            "SELECT prev_hash, row_hash FROM core.audit_logs "
            "WHERE organisation = $1 AND seq = 1",
            {org});
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "prev_hash") == "0000000000000000000000000000000000000000000000000000000000000000",
            "genesis row prev_hash must be 64 hex zeros (sentinel)");
        CHECK_MSG(!cell(r, 0, "row_hash").empty(), "genesis row_hash must not be empty");
    }

    // Spot-check: prev_hash[n] == row_hash[n-1] for all n > 1
    {
        const Row r = exec(t_conn,
            "SELECT seq, prev_hash, row_hash FROM core.audit_logs "
            "WHERE organisation = $1 ORDER BY seq",
            {org});
        CHECK(r.ok && ntuples(r) == 10);
        for (int i = 1; i < ntuples(r); ++i) {
            const std::string prev = cell(r, i, "prev_hash");
            const std::string expected = cell(r, i - 1, "row_hash");
            CHECK_MSG(
                prev == expected, ("prev_hash chain broken between seq " + std::to_string(i) + " and " + std::to_string(i + 1)).c_str());
        }
    }

    std::printf("    done (seq 1..10 valid)\n");
}

void testSqlTamperDetection(PGconn* t_conn) {
    std::printf("\n[2] SQL tamper detection (UPDATE row 5)\n");
    const std::string org = "audit_test_tamper";
    wipeOrg(t_conn, org);

    for (int i = 1; i <= 10; ++i)
        appendLog(t_conn, org, "evt." + std::to_string(i));

    // Verify baseline
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok && ntuples(r) == 1);
        CHECK(cell(r, 0, "is_valid") == "t");
    }

    // Tamper: bypass trigger with session_replication_role and update seq=5's action
    {
        exec(t_conn, "SET session_replication_role = replica");
        const Row r = exec(t_conn,
            "UPDATE core.audit_logs SET action = 'tampered' "
            "WHERE organisation = $1 AND seq = 5",
            {org});
        CHECK_MSG(r.ok, "superuser UPDATE must succeed (tamper simulation)");
        exec(t_conn, "SET session_replication_role = DEFAULT");
    }

    // Chain must now be broken at seq 5
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "is_valid") == "f", "chain must be invalid after tampering row 5");
        const std::string broken = cell(r, 0, "first_broken_seq");
        CHECK_MSG(broken == "5", ("first broken seq must be 5, got: " + broken).c_str());
    }

    std::printf("    done (tamper at seq=5 detected)\n");
}

void testSqlDeleteDetection(PGconn* t_conn) {
    std::printf("\n[3] SQL delete detection (DELETE row 3)\n");
    const std::string org = "audit_test_delete";
    wipeOrg(t_conn, org);

    for (int i = 1; i <= 10; ++i)
        appendLog(t_conn, org, "evt." + std::to_string(i));

    // Verify baseline
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok && ntuples(r) == 1);
        CHECK(cell(r, 0, "is_valid") == "t");
    }

    // Delete row with seq=3 — simulates physical deletion
    {
        exec(t_conn, "SET session_replication_role = replica");
        const Row r = exec(t_conn,
            "DELETE FROM core.audit_logs "
            "WHERE organisation = $1 AND seq = 3",
            {org});
        CHECK_MSG(r.ok, "superuser DELETE must succeed (tamper simulation)");
        exec(t_conn, "SET session_replication_role = DEFAULT");
    }

    // After deleting seq=3, row seq=4 now has prev_hash pointing to hash of
    // seq=3 (which no longer exists), so verify must detect the break at seq=4.
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "is_valid") == "f", "chain must be invalid after deleting row 3");
        const std::string broken = cell(r, 0, "first_broken_seq");
        // Broken at seq=4 (seq discontinuity: 2→4 gap, prev_hash mismatch)
        CHECK_MSG(broken == "4", ("first broken seq must be 4, got: " + broken).c_str());
    }

    std::printf("    done (delete at seq=3 detected, broken at seq=4)\n");
}

void testConcurrentWriters(PGconn* t_conn, const std::string& t_conninfo) {
    std::printf("\n[4] Concurrent 8-writer advisory-lock serialization\n");
    const std::string org = "audit_test_concurrent";
    wipeOrg(t_conn, org);

    constexpr int kWriters = 8;
    constexpr int kRowsEach = 50;
    constexpr int kTotal = kWriters * kRowsEach;

    std::vector<std::string> errors;
    std::mutex err_mutex;

    // Each thread opens its own libpq connection and fires kRowsEach appends
    std::vector<std::thread> threads;
    threads.reserve(kWriters);
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w]() {
            PGconn* conn = ::PQconnectdb(t_conninfo.c_str());
            if (::PQstatus(conn) != CONNECTION_OK) {
                std::lock_guard lock(err_mutex);
                errors.push_back("writer " + std::to_string(w) + " connect failed: " + ::PQerrorMessage(conn));
                ::PQfinish(conn);
                return;
            }
            for (int i = 0; i < kRowsEach; ++i) {
                const std::string action = "concurrent.w" + std::to_string(w) + "_i" + std::to_string(i);
                PGresult* r = nullptr;
                {
                    const char* vals[2] = {org.c_str(), action.c_str()};
                    r = ::PQexecParams(conn,
                        "SELECT core.append_audit_log("
                        "$1::text, 'user', 1, 'writer@example.com', "
                        "$2::text, 'audit_test', 'obj1', '127.0.0.1', "
                        "'{}'::jsonb, 'success')",
                        2, nullptr, vals, nullptr, nullptr, 0);
                }
                const bool ok = r && (::PQresultStatus(r) == PGRES_TUPLES_OK || ::PQresultStatus(r) == PGRES_COMMAND_OK);
                if (!ok) {
                    std::lock_guard lock(err_mutex);
                    errors.push_back("writer " + std::to_string(w) + " append failed: " + (r ? ::PQresultErrorMessage(r) : "null result"));
                }
                if (r)
                    ::PQclear(r);
            }
            ::PQfinish(conn);
        });
    }
    for (auto& t : threads)
        t.join();

    if (!errors.empty()) {
        ++g_failures;
        for (const auto& e : errors)
            std::printf("FAIL (writer error): %s\n", e.c_str());
        return;
    }

    // All kTotal rows must exist
    {
        const Row r = exec(t_conn,
            "SELECT COUNT(*) AS n FROM core.audit_logs "
            "WHERE organisation = $1",
            {org});
        CHECK(r.ok && ntuples(r) == 1);
        const std::string count = cell(r, 0, "n");
        CHECK_MSG(count == std::to_string(kTotal), ("expected " + std::to_string(kTotal) + " rows, got " + count).c_str());
    }

    // seq must be strictly monotonic 1..kTotal with no gaps
    {
        const Row r = exec(t_conn,
            "SELECT MIN(seq) AS mn, MAX(seq) AS mx, COUNT(*) AS n, "
            "COUNT(*) FILTER (WHERE seq IS NULL) AS nulls "
            "FROM core.audit_logs WHERE organisation = $1",
            {org});
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "mn") == "1", "min seq must be 1");
        CHECK_MSG(cell(r, 0, "mx") == std::to_string(kTotal),
            ("max seq must be " + std::to_string(kTotal) + ", got " + cell(r, 0, "mx")).c_str());
        CHECK_MSG(cell(r, 0, "nulls") == "0", "no null seq values");
    }

    // No duplicate seq values
    {
        const Row r = exec(t_conn,
            "SELECT COUNT(*) AS dupes FROM ("
            "  SELECT seq FROM core.audit_logs "
            "  WHERE organisation = $1 "
            "  GROUP BY seq HAVING COUNT(*) > 1"
            ") sub",
            {org});
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "dupes") == "0", "no duplicate seq values allowed");
    }

    // Chain verification must pass for all 400 rows
    {
        const Row r = verifyChain(t_conn, org);
        CHECK(r.ok && ntuples(r) == 1);
        CHECK_MSG(cell(r, 0, "is_valid") == "t", "concurrent 400-row chain must verify as valid");
    }

    std::printf("    done (%d writers × %d rows = %d total, chain valid)\n", kWriters, kRowsEach, kTotal);
}

void testEd25519Checkpoint(PGconn* t_conn) {
    std::printf("\n[5] Ed25519 checkpoint signing and verification\n");
    const std::string org = "audit_test_checkpoint";
    wipeOrg(t_conn, org);

    // Need at least one row to have a checkpoint target
    for (int i = 1; i <= 5; ++i)
        appendLog(t_conn, org, "checkpoint_seed." + std::to_string(i));

    // Fetch last_seq and last_hash for the org
    std::string last_seq_str;
    std::string last_hash;
    {
        const Row r = exec(t_conn,
            "SELECT seq::text, row_hash FROM core.audit_logs "
            "WHERE organisation = $1 ORDER BY seq DESC LIMIT 1",
            {org});
        CHECK(r.ok && ntuples(r) == 1);
        last_seq_str = cell(r, 0, "seq");
        last_hash = cell(r, 0, "row_hash");
    }
    CHECK_MSG(!last_hash.empty(), "last_hash must not be empty");

    // Verify key generator utilities from sgrn/utils/keygen.hpp
    auto kp_res = sgrn::utils::keygen::generateEd25519KeyPair();
    CHECK_MSG(kp_res.hasValue(), "sgrn::utils::keygen::generateEd25519KeyPair must succeed");
    if (kp_res.hasValue()) {
        CHECK_MSG(kp_res.value().private_key_hex.size() == 64, "Ed25519 priv key must be 64 hex chars");
        CHECK_MSG(kp_res.value().public_key_hex.size() == 64, "Ed25519 pub key must be 64 hex chars");
    }
    auto rand_key = sgrn::utils::keygen::generateRandomKeyHex(32);
    CHECK_MSG(rand_key.hasValue() && rand_key.value().size() == 64, "generateRandomKeyHex must produce 64 hex chars (32 bytes)");

    // Generate Ed25519 key pair for checkpointing
    std::string priv_hex;
    std::string pub_hex;
    const bool kp_ok = sgrn::datastore::audit::AuditLogger::generateEd25519KeyPair(priv_hex, pub_hex);
    CHECK_MSG(kp_ok, "Ed25519 key generation must succeed");
    if (!kp_ok)
        return;

    // Build checkpoint payload: "org:last_seq:last_hash:timestamp"
    const std::string ts = "2026-11-07T00:00:00Z";
    const std::string payload = org + ":" + last_seq_str + ":" + last_hash + ":" + ts;

    // Sign
    std::string sig_hex;
    const bool sign_ok = sgrn::datastore::audit::AuditLogger::signEd25519(payload, priv_hex, sig_hex);
    CHECK_MSG(sign_ok, "Ed25519 sign must succeed");
    CHECK_MSG(!sig_hex.empty(), "Ed25519 signature must not be empty");
    if (!sign_ok || sig_hex.empty())
        return;

    // Store checkpoint in core.audit_checkpoints
    {
        const Row r = exec(t_conn,
            "INSERT INTO core.audit_checkpoints "
            "(organisation, last_seq, last_hash, "
            " signature, public_key) "
            "VALUES ($1, $2::bigint, $3, $4, $5)",
            {org, last_seq_str, last_hash, sig_hex, pub_hex});
        CHECK_MSG(r.ok, "INSERT INTO audit_checkpoints must succeed");
    }

    // Verify: reconstruct payload from DB row and verify signature
    {
        const Row r = exec(t_conn,
            "SELECT last_seq::text, last_hash, "
            "created_at::text AS checkpoint_at, signature, public_key "
            "FROM core.audit_checkpoints WHERE organisation = $1 "
            "ORDER BY last_seq DESC LIMIT 1",
            {org});
        CHECK(r.ok && ntuples(r) == 1);

        const std::string db_seq = cell(r, 0, "last_seq");
        const std::string db_hash = cell(r, 0, "last_hash");
        const std::string db_ts = cell(r, 0, "checkpoint_at");
        const std::string db_sig_hex = cell(r, 0, "signature");
        const std::string db_pub_hex = cell(r, 0, "public_key");

        CHECK(db_seq == last_seq_str);
        CHECK(db_hash == last_hash);

        // Reconstruct payload (timestamp stored with possible timezone suffix — use
        // whatever the DB echoed back for a round-trip-consistent check)
        const std::string db_payload = org + ":" + db_seq + ":" + db_hash + ":" + db_ts;

        // Verify with original ts first (may differ in tz format)
        const bool sig_ok_orig = sgrn::datastore::audit::AuditLogger::verifyEd25519(payload, db_sig_hex, db_pub_hex);
        const bool sig_ok_db = sgrn::datastore::audit::AuditLogger::verifyEd25519(db_payload, db_sig_hex, db_pub_hex);
        CHECK_MSG(sig_ok_orig || sig_ok_db, "Ed25519 signature must verify correctly");

        // Now flip one byte in the signature and verify it fails
        std::string bad_sig_hex = db_sig_hex;
        if (!bad_sig_hex.empty()) {
            bad_sig_hex[0] = (bad_sig_hex[0] == 'a') ? 'b' : 'a';
        }
        const bool bad_ok_orig = sgrn::datastore::audit::AuditLogger::verifyEd25519(payload, bad_sig_hex, db_pub_hex);
        const bool bad_ok_db = sgrn::datastore::audit::AuditLogger::verifyEd25519(db_payload, bad_sig_hex, db_pub_hex);
        CHECK_MSG(!bad_ok_orig && !bad_ok_db, "corrupted Ed25519 signature must NOT verify");

        // Flip one byte in last_hash and verify signature fails
        std::string bad_hash = db_hash;
        if (!bad_hash.empty())
            bad_hash[0] = (bad_hash[0] == 'a') ? 'b' : 'a';
        const std::string bad_payload = org + ":" + db_seq + ":" + bad_hash + ":" + db_ts;
        CHECK_MSG(!sgrn::datastore::audit::AuditLogger::verifyEd25519(bad_payload, db_sig_hex, db_pub_hex),
            "corrupted hash in checkpoint payload must fail signature verification");
    }

    std::printf("    done (sign OK; corrupted sig/hash correctly rejected)\n");
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== audit-tamper-test ===\n");

    sgrn::test::PgFixture fix("sgrn_audit_tamper");
    if (!fix.available()) {
        std::printf("SKIP: %s\n", fix.skipReason().c_str());
        return 0;
    }
    std::printf("Postgres: %s\n", fix.adminConnInfo().c_str());

    PGconn* conn = ::PQconnectdb(fix.adminConnInfo().c_str());
    if (::PQstatus(conn) != CONNECTION_OK) {
        std::printf("SKIP: cannot open admin connection: %s\n", ::PQerrorMessage(conn));
        ::PQfinish(conn);
        return 0;
    }

    testSequentialAppendAndVerify(conn);
    testSqlTamperDetection(conn);
    testSqlDeleteDetection(conn);
    testConcurrentWriters(conn, fix.adminConnInfo());
    testEd25519Checkpoint(conn);

    ::PQfinish(conn);

    if (g_failures == 0) {
        std::printf("\nPASS — all audit tamper-evidence tests passed\n");
        return 0;
    }
    std::printf("\nFAIL — %d test(s) failed\n", g_failures);
    return 1;
}
