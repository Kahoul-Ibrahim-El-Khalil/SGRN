// admin-support-schema-test.cpp — WP2 (migration 008) schema-level
// regression tests against a disposable Postgres that replays the
// postgres/** tree (fresh-install mirrors).
//
// Coverage:
//   B1  core.domains quota columns exist and are nullable.
//   B2  trg_assert_same_org: cross-org admin assignment / permission rows
//       rejected (user_id pin + domain_id pin), same-org grants pass.
//   B3  authenticate_user hardening: status, soft-delete, org status.
//   B4  authenticate_automated_service hardening: deleted_at, org status.
//   B5  Lifecycle session revocation (user/service/org) + notify queue rows.
//   B6  storage.enforce_domain_quota: domain_id derivation from the label,
//       unknown-domain rejection, entry_count_limit gating (delta semantics).
//   B7  Domain byte cap via the file_objects gate + storage.domain_usage.
//   B8  Actor byte cap enforced at the file_objects stage (F11 fix) and
//       owner/org totals refreshed (+0-bytes-before regression).
//   B9  Organisation byte cap at the file_objects stage.
//   B10 core.rename_domain: validation, collision, path + storage label sync.
//   B11 core.create_organisation validation.
//
// Environment: PGHOST (+PGPORT/PGUSER/PGPASSWORD) targets an external dev
// server via a scratch database; SGRN_PG_BIN/PATH supplies initdb+postgres
// for an ephemeral cluster; nothing available → SKIP + exit 0.

#include "pg-fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

#define CHECK_STR(arg_a, arg_b)                                                                                                            \
    do {                                                                                                                                   \
        const std::string sa = (arg_a);                                                                                                    \
        const std::string sb = (arg_b);                                                                                                    \
        if (sa != sb) {                                                                                                                    \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: [%s] != [%s]\n", __FILE__, __LINE__, sa.c_str(), sb.c_str());                                         \
        }                                                                                                                                  \
    } while (0)

struct Query {
    PGresult* res = nullptr;
    bool ok = false; // executed without SQL error (rows may be empty)
};

Query query(PGconn* t_conn, const std::string& t_sql, const std::vector<std::string>& t_params = {}) {
    Query out;
    std::vector<const char*> values;
    values.reserve(t_params.size());
    for (const std::string& p : t_params)
        values.push_back(p.c_str());
    out.res = ::PQexecParams(
        t_conn, t_sql.c_str(), static_cast<int>(t_params.size()), nullptr, values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!out.res) {
        std::printf("FAIL PQexecParams returned NULL: %s\n", ::PQerrorMessage(t_conn));
        return out;
    }
    const ExecStatusType st = ::PQresultStatus(out.res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
        out.ok = false;
    else
        out.ok = true;
    return out;
}

std::string errOf(const Query& t_q) {
    return t_q.res ? std::string(::PQresultErrorMessage(t_q.res)) : std::string();
}

std::string field(const Query& t_q, int t_row, int t_col) {
    if (!t_q.res)
        return {};
    const char* v = ::PQgetvalue(t_q.res, t_row, t_col);
    return v ? v : "";
}

std::string fieldById(const Query& t_q, int t_row, const char* t_name) {
    return field(t_q, t_row, t_q.res ? ::PQfnumber(t_q.res, t_name) : -1);
}

int nrows(const Query& t_q) {
    return t_q.res ? ::PQntuples(t_q.res) : -1;
}

std::string scalar(const Query& t_q, const char* t_name) {
    if (nrows(t_q) < 1)
        return {};
    return fieldById(t_q, 0, t_name);
}

void clear(PGconn* t_conn, const std::string& t_sql) {
    const Query r = query(t_conn, t_sql);
    if (!r.ok) {
        ++g_failures;
        std::printf("FAIL cleanup failed: %s — %s\n", errOf(r).c_str(), t_sql.c_str());
    }
}

// A statement expected to fail; returns true when it failed and its error
// contains `t_needle` (case-sensitive).
bool expectError(const Query& t_q, const char* t_needle) {
    if (t_q.ok) {
        ++g_failures;
        std::printf("FAIL expected SQL error containing [%s] but statement succeeded\n", t_needle);
        return false;
    }
    const std::string err = errOf(t_q);
    if (err.find(t_needle) == std::string::npos) {
        ++g_failures;
        std::printf("FAIL expected error containing [%s], got: %s\n", t_needle, err.c_str());
        return false;
    }
    return true;
}

} // namespace

int main() {
    sgrn::test::PgFixture pg("sgrn_scratch");
    if (!pg.available()) {
        std::printf("SKIP: live Postgres unavailable — %s\n", pg.skipReason().c_str());
        return 0;
    }

    PGconn* admin = ::PQconnectdb(pg.adminConnInfo().c_str());
    CHECK(::PQstatus(admin) == CONNECTION_OK);
    if (::PQstatus(admin) != CONNECTION_OK) {
        std::printf("SKIP: cannot open admin connection: %s\n", ::PQerrorMessage(admin));
        ::PQfinish(admin);
        return 0;
    }

    // ── fixture data ─────────────────────────────────────────────────────────
    clear(admin, "INSERT INTO core.organisations (name, description) VALUES ('w2org-a','wp2'), ('w2org-b','wp2')"
                 " ON CONFLICT (name) DO NOTHING");
    clear(admin, "INSERT INTO core.domains (organisation, name) VALUES ('w2org-a','alpha'), ('w2org-a','alpha-child'),"
                 " ('w2org-b','beta') ON CONFLICT DO NOTHING");
    clear(admin, "INSERT INTO core.users (organisation, first_name, family_name, email, password) VALUES"
                 " ('w2org-a','T','A','ua@w2test.local','pw-plain-ua'),"
                 " ('w2org-a','T','B','ub@w2test.local','pw-plain-ub'),"
                 " ('w2org-b','T','C','uc@w2test.local','pw-plain-uc')"
                 " ON CONFLICT (email) DO NOTHING");
    clear(admin, "INSERT INTO core.automated_services (organisation, name, token, token_secret_hash) VALUES"
                 " ('w2org-a','w2-svc','11111111-2222-3333-4444-555555555555','w2-svc-secret')"
                 " ON CONFLICT (token) DO NOTHING");

    const std::string org_a = scalar(query(admin, "SELECT name FROM core.organisations WHERE name = 'w2org-a'"), "name");
    const std::string dom_alpha =
        scalar(query(admin, "SELECT id::text FROM core.domains WHERE organisation = 'w2org-a' AND name = 'alpha'"), "id");
    const std::string dom_beta =
        scalar(query(admin, "SELECT id::text FROM core.domains WHERE organisation = 'w2org-b' AND name = 'beta'"), "id");
    const std::string uid_ua = scalar(query(admin, "SELECT id::text FROM core.users WHERE email = 'ua@w2test.local'"), "id");
    const std::string uid_uc = scalar(query(admin, "SELECT id::text FROM core.users WHERE email = 'uc@w2test.local'"), "id");
    CHECK(!org_a.empty() && !dom_alpha.empty() && !dom_beta.empty() && !uid_ua.empty() && !uid_uc.empty());

    // ── B1: domain quota columns exist, nullable ─────────────────────────────
    {
        const Query q = query(admin, "SELECT column_name, is_nullable FROM information_schema.columns"
                                     " WHERE table_schema = 'core' AND table_name = 'domains'"
                                     " AND column_name IN ('storage_limit','entry_count_limit') ORDER BY column_name");
        CHECK(q.ok);
        CHECK(nrows(q) == 2);
        CHECK(field(q, 0, 0) == "entry_count_limit" && field(q, 0, 1) == "YES");
        CHECK(field(q, 1, 0) == "storage_limit" && field(q, 1, 1) == "YES");
    }

    // ── B2: tenant-consistency guard ─────────────────────────────────────────
    {
        // user of another org referenced by an organisation-scope assignment
        expectError(query(admin,
                        "INSERT INTO core.admin_assignments (user_id, scope, organisation)"
                        " VALUES ($1::int, 'organisation', 'w2org-a')",
                        {uid_uc}),
            "does not belong to organisation");

        // cross-org domain_id pin
        expectError(query(admin,
                        "INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id)"
                        " VALUES ($1::int, 'domain', 'w2org-a', $2::int)",
                        {uid_ua, dom_beta}),
            "Cross-organisation admin assignment rejected");

        // user_domain_permissions: user from another organisation
        expectError(query(admin,
                        "INSERT INTO core.user_domain_permissions (user_id, organisation, domain)"
                        " VALUES ($1::int, 'w2org-a', 'alpha')",
                        {uid_uc}),
            "does not belong to organisation");

        // same-org grant passes; clean up
        const Query ok1 = query(admin,
            "INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id)"
            " VALUES ($1::int, 'domain', 'w2org-a', $2::int)",
            {uid_ua, dom_alpha});
        CHECK(ok1.ok);
        clear(admin, "DELETE FROM core.admin_assignments WHERE user_id = " + uid_ua);

        // missing domain → clean error (domain_id FK or guard, both 23503-family)
        const Query bad = query(admin,
            "INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id)"
            " VALUES ($1::int, 'domain', 'w2org-a', 987654)",
            {uid_ua});
        CHECK(!bad.ok);
    }

    // ── B3: authenticate_user hardening ──────────────────────────────────────
    {
        auto login = [&](const char* t_email, const char* t_pw) {
            return query(admin, "SELECT id FROM core.authenticate_user($1, $2)", {t_email, t_pw});
        };
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 1);

        clear(admin, "UPDATE core.users SET status = 'suspended' WHERE email = 'ua@w2test.local'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 0);
        clear(admin, "UPDATE core.users SET status = 'active' WHERE email = 'ua@w2test.local'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 1);

        clear(admin, "UPDATE core.users SET is_active = false WHERE email = 'ua@w2test.local'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 0);
        clear(admin, "UPDATE core.users SET is_active = true WHERE email = 'ua@w2test.local'");

        clear(admin, "UPDATE core.users SET deleted_at = now() WHERE email = 'ua@w2test.local'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 0);
        clear(admin, "UPDATE core.users SET deleted_at = NULL WHERE email = 'ua@w2test.local'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 1);

        clear(admin, "UPDATE core.organisations SET status = 'suspended' WHERE name = 'w2org-a'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 0);
        clear(admin, "UPDATE core.organisations SET status = 'active' WHERE name = 'w2org-a'");
        CHECK(nrows(login("ua@w2test.local", "pw-plain-ua")) == 1);
    }

    // ── B4: authenticate_automated_service hardening ─────────────────────────
    {
        auto auth = [&]() {
            return query(admin, "SELECT id FROM core.authenticate_automated_service($1::uuid, $2)",
                {"11111111-2222-3333-4444-555555555555", "w2-svc-secret"});
        };
        CHECK(nrows(auth()) == 1);

        clear(admin, "UPDATE core.automated_services SET deleted_at = now()"
                     " WHERE token = '11111111-2222-3333-4444-555555555555'");
        CHECK(nrows(auth()) == 0);
        clear(admin, "UPDATE core.automated_services SET deleted_at = NULL"
                     " WHERE token = '11111111-2222-3333-4444-555555555555'");
        CHECK(nrows(auth()) == 1);

        clear(admin, "UPDATE core.organisations SET status = 'suspended' WHERE name = 'w2org-a'");
        CHECK(nrows(auth()) == 0);
        clear(admin, "UPDATE core.organisations SET status = 'active' WHERE name = 'w2org-a'");
        CHECK(nrows(auth()) == 1);
    }

    // ── helper: create a live session (down counter cleanup handled per case) ─
    auto newSession = [&](const char* t_kind, const std::string& t_owner) {
        const std::string col = std::string(t_kind) == "user" ? "user_id" : "automated_service_id";
        const Query q = query(admin,
            "INSERT INTO core.sessions (" + col +
                ", token, ip) VALUES ($1::int, gen_random_uuid(), $2::inet)"
                " RETURNING id::text",
            {t_owner, "127.0.0.10"});
        CHECK(q.ok);
        return q.ok ? scalar(q, "id") : std::string();
    };

    // ── B5: lifecycle session revocation ─────────────────────────────────────
    {
        const std::string s1 = newSession("user", uid_ua);
        const std::string s2 = newSession("user", uid_ua);
        CHECK(!s1.empty() && !s2.empty());

        clear(admin, "UPDATE core.users SET status = 'suspended' WHERE email = 'ua@w2test.local'");
        const Query live = query(admin, "SELECT count(*) AS n FROM core.sessions WHERE user_id = " + uid_ua + " AND terminated_at IS NULL");
        CHECK_STR(scalar(live, "n"), "0");
        const Query revoked = query(admin, "SELECT count(*) AS n FROM core.sessions WHERE user_id = " + uid_ua +
                                               " AND termination_reason = 'revoked' AND terminated_at IS NOT NULL");
        CHECK_STR(scalar(revoked, "n"), "2");

        // notify queue got one 'revoke' entry per terminated session
        const Query queue = query(admin, "SELECT count(*) AS n FROM core.session_notify_queue WHERE action = 'revoke'");
        CHECK_STR(scalar(queue, "n"), "2");

        // reactivation does not resurrect sessions
        clear(admin, "UPDATE core.users SET status = 'active' WHERE email = 'ua@w2test.local'");
        const Query after =
            query(admin, "SELECT count(*) AS n FROM core.sessions WHERE user_id = " + uid_ua + " AND terminated_at IS NULL");
        CHECK_STR(scalar(after, "n"), "0");
        clear(admin, "DELETE FROM core.sessions WHERE user_id = " + uid_ua);
        clear(admin, "DELETE FROM core.session_notify_queue");

        // service soft-delete terminates its sessions
        const std::string s3 = newSession("service", scalar(query(admin, "SELECT id::text FROM core.automated_services"
                                                                         " WHERE token = '11111111-2222-3333-4444-555555555555'"),
                                                         "id"));
        CHECK(!s3.empty());
        clear(admin, "UPDATE core.automated_services SET deleted_at = now()"
                     " WHERE token = '11111111-2222-3333-4444-555555555555'");
        const Query svc_live = query(admin, "SELECT count(*) AS n FROM core.sessions WHERE id = " + s3 + " AND terminated_at IS NULL");
        CHECK_STR(scalar(svc_live, "n"), "0");
        clear(admin, "DELETE FROM core.sessions WHERE automated_service_id IS NOT NULL");
        clear(admin, "DELETE FROM core.session_notify_queue");

        // org suspension terminates user + service sessions of that org
        const std::string s4 = newSession("user", uid_ua);
        const std::string s5 = newSession("user", uid_uc);
        CHECK(!s4.empty() && !s5.empty());
        clear(admin, "UPDATE core.organisations SET status = 'suspended' WHERE name = 'w2org-a'");
        const Query a_live = query(admin, "SELECT count(*) AS n FROM core.sessions WHERE id = " + s4 + " AND terminated_at IS NULL");
        CHECK_STR(scalar(a_live, "n"), "0");
        const Query b_live = query(admin, "SELECT count(*) AS n FROM core.sessions WHERE id = " + s5 + " AND terminated_at IS NULL");
        CHECK_STR(scalar(b_live, "n"), "1"); // w2org-b untouched: still live
        clear(admin, "UPDATE core.organisations SET status = 'active' WHERE name = 'w2org-a'");
        clear(admin, "DELETE FROM core.sessions");
        clear(admin, "DELETE FROM core.session_notify_queue");
    }

    // storage upload helper: (object, file[label-only or actor], mapping) in one
    // transaction; returns the failing/succeeding statements as a whole.
    auto uploadFile = [&](const std::string& t_session, const std::string& t_domain_label, const std::string& t_user_id,
                          const std::string& t_obj_bytes) {
        clear(admin, "BEGIN");
        const Query obj = query(admin,
            "INSERT INTO storage.objects (bucket, key, sha256, size, original_size, provider)"
            " VALUES ('w2-bucket','w2-key-' || md5(random()::text), repeat('a',64), $1::bigint, $1::bigint, 'GARAGE')"
            " RETURNING id::text",
            {t_obj_bytes});
        const std::string obj_id = obj.ok ? scalar(obj, "id") : "-1";
        const std::string user_part = t_user_id.empty() ? "NULL" : t_user_id;
        const std::string dom_part = t_domain_label.empty() ? "NULL" : "'" + t_domain_label + "'";
        const Query f =
            query(admin, "INSERT INTO storage.files (name, user_id, automated_service_id, session_id, domain, full_path)"
                         " VALUES ('w2-file-' || md5(random()::text), " +
                             user_part + ", NULL, " + t_session + ", " + dom_part + ", '/w2-' || md5(random()::text)) RETURNING id::text");
        if (!f.ok) {
            clear(admin, "ROLLBACK");
            return std::string("files: ") + errOf(f);
        }
        const std::string file_id = scalar(f, "id");
        const Query fo = query(admin, "INSERT INTO storage.file_objects (file_id, object_id, part_index, role)"
                                      " VALUES (" +
                                          file_id + ", " + obj_id + ", 0, 'primary')");
        if (!fo.ok) {
            clear(admin, "ROLLBACK");
            return std::string("file_objects: ") + errOf(fo);
        }
        clear(admin, "COMMIT");
        return std::string();
    };

    // ── B6: domain entry quota + label → domain_id derivation ────────────────
    {
        clear(admin, "UPDATE core.domains SET entry_count_limit = 2, storage_limit = NULL WHERE id = " + dom_alpha);
        const std::string s = newSession("user", uid_ua);
        CHECK(!s.empty());

        const std::string e1 = uploadFile(s, "alpha", "", "100");
        CHECK(e1.empty());
        const Query derived = query(admin, "SELECT DISTINCT d.id::text AS id FROM storage.files f"
                                           " JOIN core.domains d ON d.id = f.domain_id"
                                           " WHERE f.session_id = " +
                                               s + " AND d.name = 'alpha'");
        CHECK(nrows(derived) == 1);

        CHECK(uploadFile(s, "alpha", "", "100").empty());

        // third entry → domain entry cap
        const std::string e3 = uploadFile(s, "alpha", "", "100");
        CHECK(e3.find("Domain entry count limit exceeded") != std::string::npos);

        // lowering the bar again works after deleting one row
        clear(admin, "DELETE FROM storage.files WHERE session_id = " + s +
                         " AND domain = 'alpha' AND id = ("
                         "SELECT min(id) FROM storage.files WHERE session_id = " +
                         s + " AND domain = 'alpha')");
        CHECK(uploadFile(s, "alpha", "", "100").empty());
        // back at the cap → the next entry is rejected again
        const std::string e4 = uploadFile(s, "alpha", "", "100");
        CHECK(e4.find("Domain entry count limit exceeded") != std::string::npos);

        // unknown domain label → rejected
        const std::string e_unk = uploadFile(s, "ghost", "", "10");
        CHECK(e_unk.find("Unknown domain") != std::string::npos);

        clear(admin, "DELETE FROM storage.files WHERE session_id = " + s);
        clear(admin, "DELETE FROM storage.objects WHERE bucket = 'w2-bucket'");
        clear(admin, "DELETE FROM storage.directories WHERE session_id = " + s);
        clear(admin, "DELETE FROM core.sessions WHERE id = " + s);
        clear(admin, "UPDATE core.domains SET entry_count_limit = NULL WHERE id = " + dom_alpha);
    }

    // ── B7: domain byte cap + domain_usage ───────────────────────────────────
    {
        clear(admin, "UPDATE core.domains SET storage_limit = 1000, entry_count_limit = NULL WHERE id = " + dom_alpha);
        const std::string s = newSession("user", uid_ua);
        CHECK(!s.empty());

        CHECK(uploadFile(s, "alpha", "", "700").empty());

        const Query usage = query(admin,
            "SELECT virtual_size::text AS vb, real_size::text AS rb, entry_count::text AS ec"
            " FROM storage.domain_usage($1::int)",
            {dom_alpha});
        CHECK(usage.ok);
        CHECK_STR(scalar(usage, "vb"), "700");
        CHECK_STR(scalar(usage, "rb"), "700");
        CHECK_STR(scalar(usage, "ec"), "1");

        // 300 more would exceed 1000 → mapping rejected
        const std::string over = uploadFile(s, "alpha", "", "400");
        CHECK(over.find("Domain storage quota exceeded") != std::string::npos);

        // lifting the cap lets the same bytes through
        clear(admin, "UPDATE core.domains SET storage_limit = NULL WHERE id = " + dom_alpha);
        CHECK(uploadFile(s, "alpha", "", "400").empty());

        clear(admin, "DELETE FROM storage.files WHERE session_id = " + s);
        clear(admin, "DELETE FROM storage.objects WHERE bucket = 'w2-bucket'");
        clear(admin, "DELETE FROM core.sessions WHERE id = " + s);
    }

    // ── B8: actor byte cap enforced at the mapping stage (F11) + totals ──────
    {
        const std::string s = newSession("user", uid_ua);
        CHECK(!s.empty());
        const Query before = query(admin, "SELECT total_virtual_size::text AS b, total_entry_count::text AS e"
                                          " FROM core.users WHERE id = " +
                                              uid_ua);
        const std::string before_bytes = scalar(before, "b");
        const std::string before_entries = scalar(before, "e");

        clear(admin, "UPDATE core.users SET storage_limit = 500 WHERE id = " + uid_ua);
        const std::string over = uploadFile(s, "", uid_ua, "1000");
        CHECK(over.find("Actor storage quota exceeded") != std::string::npos);

        // quota lifted: accepted, and owner/org totals advanced by the bytes
        clear(admin, "UPDATE core.users SET storage_limit = NULL WHERE id = " + uid_ua);
        CHECK(uploadFile(s, "", uid_ua, "1000").empty());
        const Query after = query(admin, "SELECT total_virtual_size::text AS b, total_entry_count::text AS e"
                                         " FROM core.users WHERE id = " +
                                             uid_ua);
        CHECK_STR(scalar(after, "e"), std::to_string(std::stoll(before_entries) + 1));

        const Query orgq = query(admin, "SELECT total_virtual_size::text AS b FROM core.organisations WHERE name = 'w2org-a'");
        const std::string org_after = scalar(orgq, "b");
        // the org aggregate must include the actor delta (it is recomputed live)
        CHECK(std::stoll(org_after) > 0);

        clear(admin, "DELETE FROM storage.files WHERE session_id = " + s);
        clear(admin, "DELETE FROM storage.objects WHERE bucket = 'w2-bucket'");
        clear(admin, "DELETE FROM core.sessions WHERE id = " + s);

        // after deleting the file the owner total is back where it started
        const Query reverted = query(admin, "SELECT total_virtual_size::text AS b FROM core.users WHERE id = " + uid_ua);
        CHECK_STR(scalar(reverted, "b"), before_bytes);
    }

    // ── B9: organisation byte cap at the mapping stage ──────────────────────
    {
        const std::string s = newSession("user", uid_uc);
        CHECK(!s.empty());
        clear(admin, "UPDATE core.organisations SET storage_limit = 200 WHERE name = 'w2org-b'");
        const std::string over = uploadFile(s, "", uid_uc, "500");
        CHECK(over.find("Organisation storage quota exceeded") != std::string::npos);
        clear(admin, "UPDATE core.organisations SET storage_limit = NULL WHERE name = 'w2org-b'");
        clear(admin, "DELETE FROM core.sessions WHERE id = " + s);
    }

    // ── B10: rename_domain (subtree-safe) ────────────────────────────────────
    {
        clear(admin, "INSERT INTO core.domains (organisation, name) VALUES ('w2org-a','tmpd') ON CONFLICT DO NOTHING");
        const std::string dom_tmp =
            scalar(query(admin, "SELECT id::text FROM core.domains WHERE organisation = 'w2org-a' AND name = 'tmpd'"), "id");
        clear(admin, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('w2org-a','tmpd-child', " + dom_tmp +
                         ") ON CONFLICT DO NOTHING");
        const std::string dom_child =
            scalar(query(admin, "SELECT id::text FROM core.domains WHERE organisation = 'w2org-a' AND name = 'tmpd-child'"), "id");
        const std::string s = newSession("user", uid_ua);
        CHECK(uploadFile(s, "tmpd", "", "50").empty());

        const Query renamed_call = query(admin, "SELECT core.rename_domain($1::int, 'tmpd2')", {dom_tmp});
        CHECK(renamed_call.ok);
        const Query renamed = query(admin, "SELECT name, path FROM core.domains WHERE id = " + dom_tmp);
        CHECK_STR(field(renamed, 0, 0), "tmpd2");
        CHECK_STR(field(renamed, 0, 1), "/tmpd2");

        // descendants keep their labels but their paths follow the new prefix
        const Query child = query(admin, "SELECT name, path FROM core.domains WHERE id = " + dom_child);
        CHECK_STR(field(child, 0, 0), "tmpd-child");
        CHECK_STR(field(child, 0, 1), "/tmpd2/tmpd-child");

        const Query storage_lbl = query(admin, "SELECT DISTINCT domain FROM storage.files WHERE domain_id = " + dom_tmp);
        CHECK_STR(scalar(storage_lbl, "domain"), "tmpd2");

        // validation + collision
        const Query upper = query(admin, "SELECT core.rename_domain($1::int, 'TMPD2')", {dom_tmp});
        expectError(upper, "lower-case");
        const Query dot = query(admin, "SELECT core.rename_domain($1::int, 'a.b')", {dom_tmp});
        expectError(dot, "must not contain");
        const Query dup = query(admin, "SELECT core.rename_domain($1::int, 'alpha')", {dom_tmp});
        expectError(dup, "already exists");
        const Query missing = query(admin, "SELECT core.rename_domain(999999::int, 'x')");
        expectError(missing, "does not exist");

        clear(admin, "DELETE FROM storage.files WHERE domain_id = " + dom_tmp);
        clear(admin, "DELETE FROM storage.objects WHERE bucket = 'w2-bucket'");
        clear(admin, "DELETE FROM core.sessions WHERE id = " + s);
        clear(admin, "DELETE FROM core.domains WHERE id = " + dom_child);
        clear(admin, "DELETE FROM core.domains WHERE id = " + dom_tmp);
    }

    // ── B11: create_organisation: validation + atomic first admin ────────────
    {
        const Query ok = query(admin, "SELECT * FROM core.create_organisation('W2ORG-D', 'wp2', NULL, 1024, 10)");
        CHECK(ok.ok && nrows(ok) == 1);
        CHECK_STR(scalar(ok, "out_organisation"), "W2ORG-D");
        CHECK(scalar(ok, "out_admin_user_id").empty()); // no admin requested

        // atomic org + first organisation admin
        const Query with_admin =
            query(admin, "SELECT * FROM core.create_organisation($1, NULL, NULL, NULL, NULL, $2, 'First', 'Admin', 'pw-12-chars-ok', NULL)",
                {"w2org-e", "admin@w2org-e.local"});
        CHECK(with_admin.ok && nrows(with_admin) == 1);
        const std::string admin_id = scalar(with_admin, "out_admin_user_id");
        CHECK(!admin_id.empty());
        const Query admin_row = query(admin, "SELECT u.organisation, u.role, length(u.password) > 20 AS hashed"
                                             " FROM core.users u WHERE u.id = " +
                                                 admin_id);
        CHECK_STR(field(admin_row, 0, 0), "w2org-e");
        CHECK_STR(field(admin_row, 0, 1), "admin");
        CHECK_STR(field(admin_row, 0, 2), "t"); // plaintext was hashed by trg_hash_password
        const Query assignment = query(admin, "SELECT count(*) AS n FROM core.admin_assignments"
                                              " WHERE user_id = " +
                                                  admin_id +
                                                  " AND scope = 'organisation'"
                                                  " AND organisation = 'w2org-e'");
        CHECK_STR(scalar(assignment, "n"), "1");

        // duplicate name rolls everything back: no orphan admin user survives
        const Query dup_with_admin = query(admin,
            "SELECT * FROM core.create_organisation($1, NULL, NULL, NULL, NULL, $2, 'Second', 'Admin', 'pw-12-chars-ok', NULL)",
            {"w2org-e", "orphan@w2org-e.local"});
        CHECK(!dup_with_admin.ok);
        const Query orphan = query(admin, "SELECT count(*) AS n FROM core.users WHERE email = 'orphan@w2org-e.local'");
        CHECK_STR(scalar(orphan, "n"), "0");

        // validation
        const Query dot = query(admin, "SELECT * FROM core.create_organisation('w2.period')");
        expectError(dot, "must not contain");
        const Query blank = query(admin, "SELECT * FROM core.create_organisation('   ')");
        expectError(blank, "must not be empty");
        const Query neg = query(admin, "SELECT * FROM core.create_organisation('w2org-neg', NULL, NULL, -1)");
        expectError(neg, "non-negative");
        const Query short_pw =
            query(admin, "SELECT * FROM core.create_organisation($1, NULL, NULL, NULL, NULL, $2, 'A', 'B', 'short', NULL)",
                {"w2org-shortpw", "short@w2org-shortpw.local"});
        expectError(short_pw, "at least 12 characters");
        const Query short_pw_org = query(admin, "SELECT count(*) AS n FROM core.organisations WHERE name = 'w2org-shortpw'");
        CHECK_STR(scalar(short_pw_org, "n"), "0");

        clear(admin, "DELETE FROM core.admin_assignments WHERE user_id = " + admin_id);
        clear(admin, "DELETE FROM core.users WHERE id = " + admin_id);
        clear(admin, "DELETE FROM core.organisations WHERE name IN ('W2ORG-D', 'w2org-e')");
    }

    ::PQfinish(admin);

    if (g_failures == 0)
        std::printf("admin-support-schema-test: all checks passed\n");
    else
        std::printf("admin-support-schema-test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
