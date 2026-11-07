/*sgrn/lib/datastore/tests/scoped-admins-test.cpp*/
#include "pg-fixture.hpp"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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

struct Query {
    PGresult* res = nullptr;
    bool ok = false;
};

Query query(PGconn* t_conn, const std::string& t_sql, const std::vector<std::string>& t_params = {}) {
    Query out;
    std::vector<const char*> values;
    values.reserve(t_params.size());
    for (const std::string& p : t_params) {
        values.push_back(p.c_str());
    }
    out.res = ::PQexecParams(
        t_conn, t_sql.c_str(), static_cast<int>(t_params.size()), nullptr, values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!out.res) {
        std::printf("FAIL PQexecParams returned NULL: %s\n", ::PQerrorMessage(t_conn));
        return out;
    }
    const ExecStatusType st = ::PQresultStatus(out.res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        out.ok = false;
    } else {
        out.ok = true;
    }
    return out;
}

std::string field(const Query& t_q, int t_row, int t_col) {
    if (!t_q.res)
        return {};
    const char* v = ::PQgetvalue(t_q.res, t_row, t_col);
    return v ? v : "";
}

int nrows(const Query& t_q) {
    return t_q.res ? ::PQntuples(t_q.res) : -1;
}

std::string readFile(const std::string& t_path) {
    std::ifstream f(t_path);
    if (!f.is_open())
        return {};
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

bool execScript(PGconn* conn, const std::string& sql, std::string& err) {
    PGresult* res = ::PQexec(conn, sql.c_str());
    if (!res) {
        err = ::PQerrorMessage(conn);
        return false;
    }
    const ExecStatusType st = ::PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        err = ::PQresultErrorMessage(res);
        ::PQclear(res);
        return false;
    }
    ::PQclear(res);
    return true;
}

void testMigrationIdempotency(PGconn* conn, const std::string& migration_sql) {
    std::printf("--- 1. Testing migration idempotency (run 003 twice) ---\n");
    std::string err;
    // Run 1
    bool ok1 = execScript(conn, migration_sql, err);
    CHECK(ok1);
    if (!ok1) {
        std::printf("FAIL migration run 1 failed: %s\n", err.c_str());
    }

    // Run 2 (idempotency check)
    bool ok2 = execScript(conn, migration_sql, err);
    CHECK(ok2);
    if (!ok2) {
        std::printf("FAIL migration run 2 failed: %s\n", err.c_str());
    }
}

void testPlatformAdmin(PGconn* conn) {
    std::printf("--- 2. Testing platform admin resolution ---\n");

    // The seed admin user admin@local.com should be is_platform_admin = true
    Query q_admin = query(conn, "SELECT id FROM core.users WHERE email = $1", {"admin@local.com"});
    CHECK(q_admin.ok && nrows(q_admin) == 1);
    const std::string admin_id = field(q_admin, 0, 0);
    if (q_admin.res)
        ::PQclear(q_admin.res);

    // Level for own org -> 3
    Query q_level_own = query(conn, "SELECT core.admin_level($1::int, $2, NULL)", {admin_id, "CIC-Moulins Guelma"});
    CHECK(q_level_own.ok && nrows(q_level_own) == 1);
    CHECK(field(q_level_own, 0, 0) == "3");
    if (q_level_own.res)
        ::PQclear(q_level_own.res);

    // Level for foreign org -> 3 (platform admin is fleet-wide)
    Query q_level_for = query(conn, "SELECT core.admin_level($1::int, $2, NULL)", {admin_id, "ForeignOrg"});
    CHECK(q_level_for.ok && nrows(q_level_for) == 1);
    CHECK(field(q_level_for, 0, 0) == "3");
    if (q_level_for.res)
        ::PQclear(q_level_for.res);
}

void testOrganisationAdmin(PGconn* conn) {
    std::printf("--- 3. Testing organisation admin resolution & wrong organisation rejection ---\n");

    // Create OrgA and OrgB
    query(conn, "INSERT INTO core.organisations (name) VALUES ('OrgA'), ('OrgB') ON CONFLICT DO NOTHING");

    // Create user in OrgA
    Query q_user = query(conn, "INSERT INTO core.users (organisation, first_name, family_name, email, password, role) "
                               "VALUES ('OrgA', 'Alice', 'OrgAdmin', 'alice@orga.com', 'pwd123', 'admin') RETURNING id");
    CHECK(q_user.ok && nrows(q_user) == 1);
    const std::string alice_id = field(q_user, 0, 0);
    if (q_user.res)
        ::PQclear(q_user.res);

    // Grant organisation admin assignment to Alice for OrgA
    Query q_grant = query(conn,
        "INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id) "
        "VALUES ($1::int, 'organisation', 'OrgA', NULL)",
        {alice_id});
    CHECK(q_grant.ok);
    if (q_grant.res)
        ::PQclear(q_grant.res);

    // Target OrgA -> 2 (Organisation Admin)
    Query q_level_a = query(conn, "SELECT core.admin_level($1::int, 'OrgA', NULL)", {alice_id});
    CHECK(q_level_a.ok && nrows(q_level_a) == 1);
    CHECK(field(q_level_a, 0, 0) == "2");
    if (q_level_a.res)
        ::PQclear(q_level_a.res);

    // Target OrgB (wrong organisation) -> 0 (None)
    Query q_level_b = query(conn, "SELECT core.admin_level($1::int, 'OrgB', NULL)", {alice_id});
    CHECK(q_level_b.ok && nrows(q_level_b) == 1);
    CHECK(field(q_level_b, 0, 0) == "0");
    if (q_level_b.res)
        ::PQclear(q_level_b.res);
}

void testDomainAdminHierarchy(PGconn* conn) {
    std::printf("--- 4. Testing domain admin on subtree, child, sibling, and parent ---\n");

    // 1. Set up domain hierarchy in OrgA:
    //    /Production (id: prod_id)
    //      /Production/Line1 (id: line1_id)
    //        /Production/Line1/CellA (id: cell_a_id)
    //      /Production/Line2 (id: line2_id)
    //    /Maintenance (id: maint_id)
    Query q_prod =
        query(conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'Production', NULL) RETURNING id, path");
    CHECK(q_prod.ok && nrows(q_prod) == 1);
    const std::string prod_id = field(q_prod, 0, 0);
    CHECK(field(q_prod, 0, 1) == "/Production");
    if (q_prod.res)
        ::PQclear(q_prod.res);

    Query q_line1 = query(
        conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'Line1', $1::int) RETURNING id, path", {prod_id});
    CHECK(q_line1.ok && nrows(q_line1) == 1);
    const std::string line1_id = field(q_line1, 0, 0);
    CHECK(field(q_line1, 0, 1) == "/Production/Line1");
    if (q_line1.res)
        ::PQclear(q_line1.res);

    Query q_cell_a = query(
        conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'CellA', $1::int) RETURNING id, path", {line1_id});
    CHECK(q_cell_a.ok && nrows(q_cell_a) == 1);
    const std::string cell_a_id = field(q_cell_a, 0, 0);
    CHECK(field(q_cell_a, 0, 1) == "/Production/Line1/CellA");
    if (q_cell_a.res)
        ::PQclear(q_cell_a.res);

    Query q_line2 = query(
        conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'Line2', $1::int) RETURNING id, path", {prod_id});
    CHECK(q_line2.ok && nrows(q_line2) == 1);
    const std::string line2_id = field(q_line2, 0, 0);
    CHECK(field(q_line2, 0, 1) == "/Production/Line2");
    if (q_line2.res)
        ::PQclear(q_line2.res);

    Query q_maint =
        query(conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'Maintenance', NULL) RETURNING id, path");
    CHECK(q_maint.ok && nrows(q_maint) == 1);
    const std::string maint_id = field(q_maint, 0, 0);
    CHECK(field(q_maint, 0, 1) == "/Maintenance");
    if (q_maint.res)
        ::PQclear(q_maint.res);

    // 2. Create user Bob in OrgA with domain admin grant on Line1
    Query q_bob = query(conn, "INSERT INTO core.users (organisation, first_name, family_name, email, password, role) "
                              "VALUES ('OrgA', 'Bob', 'LineAdmin', 'bob@orga.com', 'pwd123', 'user') RETURNING id");
    CHECK(q_bob.ok && nrows(q_bob) == 1);
    const std::string bob_id = field(q_bob, 0, 0);
    if (q_bob.res)
        ::PQclear(q_bob.res);

    Query q_grant = query(conn,
        "INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id) "
        "VALUES ($1::int, 'domain', 'OrgA', $2::int)",
        {bob_id, line1_id});
    CHECK(q_grant.ok);
    if (q_grant.res)
        ::PQclear(q_grant.res);

    // 3. Test permissions from Line1:
    // a. On own subtree (Line1) -> 1
    Query q_own = query(conn, "SELECT core.admin_level($1::int, 'OrgA', $2::int)", {bob_id, line1_id});
    CHECK(q_own.ok && nrows(q_own) == 1);
    CHECK(field(q_own, 0, 0) == "1");
    if (q_own.res)
        ::PQclear(q_own.res);

    // b. On child / descendant (CellA) -> 1
    Query q_child = query(conn, "SELECT core.admin_level($1::int, 'OrgA', $2::int)", {bob_id, cell_a_id});
    CHECK(q_child.ok && nrows(q_child) == 1);
    CHECK(field(q_child, 0, 0) == "1");
    if (q_child.res)
        ::PQclear(q_child.res);

    // c. On sibling (Line2) -> 0
    Query q_sibling = query(conn, "SELECT core.admin_level($1::int, 'OrgA', $2::int)", {bob_id, line2_id});
    CHECK(q_sibling.ok && nrows(q_sibling) == 1);
    CHECK(field(q_sibling, 0, 0) == "0");
    if (q_sibling.res)
        ::PQclear(q_sibling.res);

    // d. On parent (Production) -> 0
    Query q_parent = query(conn, "SELECT core.admin_level($1::int, 'OrgA', $2::int)", {bob_id, prod_id});
    CHECK(q_parent.ok && nrows(q_parent) == 1);
    CHECK(field(q_parent, 0, 0) == "0");
    if (q_parent.res)
        ::PQclear(q_parent.res);

    // e. On unrelated root (Maintenance) -> 0
    Query q_unrelated = query(conn, "SELECT core.admin_level($1::int, 'OrgA', $2::int)", {bob_id, maint_id});
    CHECK(q_unrelated.ok && nrows(q_unrelated) == 1);
    CHECK(field(q_unrelated, 0, 0) == "0");
    if (q_unrelated.res)
        ::PQclear(q_unrelated.res);

    // f. On wrong organisation -> 0
    Query q_wrong_org = query(conn, "SELECT core.admin_level($1::int, 'OrgB', $2::int)", {bob_id, line1_id});
    CHECK(q_wrong_org.ok && nrows(q_wrong_org) == 1);
    CHECK(field(q_wrong_org, 0, 0) == "0");
    if (q_wrong_org.res)
        ::PQclear(q_wrong_org.res);
}

void testSubtreeRelocationAndRestrictions(PGconn* conn) {
    std::printf("--- 5. Testing move_domain_subtree and delete restriction ---\n");

    // Fetch line2_id, maint_id, prod_id
    Query q_domains = query(conn, "SELECT id, name FROM core.domains WHERE organisation = 'OrgA'");
    std::string prod_id, line1_id, line2_id, maint_id, cell_a_id;
    for (int i = 0; i < nrows(q_domains); ++i) {
        std::string n = field(q_domains, i, 1);
        if (n == "Production")
            prod_id = field(q_domains, i, 0);
        else if (n == "Line1")
            line1_id = field(q_domains, i, 0);
        else if (n == "Line2")
            line2_id = field(q_domains, i, 0);
        else if (n == "Maintenance")
            maint_id = field(q_domains, i, 0);
        else if (n == "CellA")
            cell_a_id = field(q_domains, i, 0);
    }
    if (q_domains.res)
        ::PQclear(q_domains.res);

    // 1. Move Line2 from Production to Maintenance
    Query q_move = query(conn, "SELECT core.move_domain_subtree($1::int, $2::int)", {line2_id, maint_id});
    CHECK(q_move.ok);
    if (q_move.res)
        ::PQclear(q_move.res);

    Query q_check_l2 = query(conn, "SELECT path FROM core.domains WHERE id = $1::int", {line2_id});
    CHECK(q_check_l2.ok && nrows(q_check_l2) == 1);
    CHECK(field(q_check_l2, 0, 0) == "/Maintenance/Line2");
    if (q_check_l2.res)
        ::PQclear(q_check_l2.res);

    // Add child Packer under Line2
    Query q_pack = query(
        conn, "INSERT INTO core.domains (organisation, name, parent_id) VALUES ('OrgA', 'Packer', $1::int) RETURNING id, path", {line2_id});
    CHECK(q_pack.ok && nrows(q_pack) == 1);
    const std::string pack_id = field(q_pack, 0, 0);
    CHECK(field(q_pack, 0, 1) == "/Maintenance/Line2/Packer");
    if (q_pack.res)
        ::PQclear(q_pack.res);

    // Move Line2 to root (parent_id = NULL)
    Query q_move_root = query(conn, "SELECT core.move_domain_subtree($1::int, NULL)", {line2_id});
    CHECK(q_move_root.ok);
    if (q_move_root.res)
        ::PQclear(q_move_root.res);

    // Both Line2 and its descendant Packer must reflect the new base path atomically
    Query q_chk_l2_root = query(conn, "SELECT path FROM core.domains WHERE id = $1::int", {line2_id});
    CHECK(q_chk_l2_root.ok && nrows(q_chk_l2_root) == 1);
    CHECK(field(q_chk_l2_root, 0, 0) == "/Line2");
    if (q_chk_l2_root.res)
        ::PQclear(q_chk_l2_root.res);

    Query q_chk_pack = query(conn, "SELECT path FROM core.domains WHERE id = $1::int", {pack_id});
    CHECK(q_chk_pack.ok && nrows(q_chk_pack) == 1);
    CHECK(field(q_chk_pack, 0, 0) == "/Line2/Packer");
    if (q_chk_pack.res)
        ::PQclear(q_chk_pack.res);

    // 2. Cycle detection: moving Line2 into its own child Packer must be rejected!
    Query q_cycle = query(conn, "SELECT core.move_domain_subtree($1::int, $2::int)", {line2_id, pack_id});
    CHECK(!q_cycle.ok); // Expected SQL error
    if (q_cycle.res)
        ::PQclear(q_cycle.res);

    // 3. Delete restriction: deleting a domain that has children must be refused!
    // Production has Line1 -> DELETE must fail with FK restrict violation
    Query q_del = query(conn, "DELETE FROM core.domains WHERE id = $1::int", {prod_id});
    CHECK(!q_del.ok); // Expected FK restrict violation
    if (q_del.res)
        ::PQclear(q_del.res);
}

} // namespace

int main() {
    std::printf("============================================================\n");
    std::printf("Running scoped_admins_test (Prompt 4)\n");
    std::printf("============================================================\n");

    sgrn::test::PgFixture pg("sgrn_scoped_admins_test");
    if (!pg.available()) {
        std::printf("SKIP: live Postgres unavailable — %s\n", pg.skipReason().c_str());
        return 0;
    }

    PGconn* admin = ::PQconnectdb(pg.adminConnInfo().c_str());
    CHECK(::PQstatus(admin) == CONNECTION_OK);
    if (::PQstatus(admin) != CONNECTION_OK) {
        std::printf("SKIP: cannot open admin connection: %s\n", ::PQerrorMessage(admin));
        if (admin)
            ::PQfinish(admin);
        return 0;
    }

    // Locate and load 003_scoped_admins.sql
    const std::string migration_path = SGRN_TEST_SOURCE_DIR "/sgrn/lib/datastore/migrations/003_scoped_admins.sql";
    std::string migration_sql = readFile(migration_path);
    if (migration_sql.empty()) {
        // Fallback relative path
        migration_sql = readFile("sgrn/lib/datastore/migrations/003_scoped_admins.sql");
    }
    CHECK(!migration_sql.empty());

    testMigrationIdempotency(admin, migration_sql);
    testPlatformAdmin(admin);
    testOrganisationAdmin(admin);
    testDomainAdminHierarchy(admin);
    testSubtreeRelocationAndRestrictions(admin);

    ::PQfinish(admin);

    if (g_failures == 0) {
        std::printf("scoped_admins_test: ALL CHECKS PASSED\n");
        return 0;
    }

    std::fprintf(stderr, "scoped_admins_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
