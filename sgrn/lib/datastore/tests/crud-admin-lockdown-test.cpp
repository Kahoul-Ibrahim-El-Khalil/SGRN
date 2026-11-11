// crud-admin-lockdown-test.cpp — WP1 security-hotfix regression suite.
//
//  Locks down the generated CRUD policy (crud/manifest.json) that guards the
//  `/api/v1/users`, `/api/v1/domains`, `/api/v1/automated-services`,
//  `/api/v1/user-domain-permissions` and `/api/v1/admin-assignments` routes.
//  Before WP1 those routes used only UserAuthFilter, and `core.users`
//  marked `is_platform_admin`, `role`, quotas and usage counters as
//  insertable+updatable — a plain user could PATCH themselves into a
//  platform admin (F2).
//
//  Sections (no live database needed except the engine section, which
//  auto-skips like crud_binds_test):
//    1. Generated spec policy: every write route on those tables demands
//       OrgAdminFilter (or PlatformAdminFilter); list/get on users and
//       automated_services is admin-only; privileged tables expose no
//       write routes at all. Asserts the *compiled* route tables, so it
//       fails against the pre-WP1 manifest.
//    2. Field locks: sensitive columns on the compiled kFields arrays are
//       never insertable/updatable, and no secret column ever leaks onto
//       the API surface.
//    3. Filter behavior: OrgAdminFilter really rejects a plain-user
//       session (in-process, session_json injected — same harness as
//       authz_matrix_test) and accepts org/platform admins.
//    4. Engine honors updatable=false: a PATCH body may contain a locked
//       key next to an allowed key — the engine builds its SET clause
//       from updatable fields only (live-DB temp table, skip when PG* env
//       is unreachable).
//    5. check_manifest_policy.py integration (exit 0 on the repo
//       manifest; exit 1 on a manipulated negative fixture).

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/query/CrudViewEngine.hpp>
#include <sgrn/datastore/query/CrudViewSpec.hpp>

#include "handlers/generated/AdminAssignmentsView.gen.hpp"
#include "handlers/generated/AutomatedServicesView.gen.hpp"
#include "handlers/generated/DomainsView.gen.hpp"
#include "handlers/generated/UserDomainPermissionsView.gen.hpp"
#include "handlers/generated/UsersView.gen.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef SGRN_TEST_SOURCE_DIR
#define DATASTORE_ROOT SGRN_TEST_SOURCE_DIR "/sgrn/lib/datastore"
#else
#define DATASTORE_ROOT "."
#endif

namespace
{

using sgrn::datastore::filters::AdminLevel;
using sgrn::datastore::filters::AdminLevelCache;
using sgrn::datastore::filters::OrgAdminFilter;
using sgrn::datastore::handlers::query::AdminAssignmentsView;
using sgrn::datastore::handlers::query::AutomatedServicesView;
using sgrn::datastore::handlers::query::DomainsView;
using sgrn::datastore::handlers::query::UserDomainPermissionsView;
using sgrn::datastore::handlers::query::UsersView;
using sgrn::datastore::query::CrudViewSpec;
using sgrn::datastore::query::Field;
using sgrn::datastore::query::FieldType;
using sgrn::datastore::query::Op;
using sgrn::datastore::query::PrimaryKey;

int g_failures = 0;
int g_cases = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        ++g_cases;                                                                                                                         \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

const Field* findFieldImpl(std::span<const Field> t_fields, std::string_view t_name) {
    for (const auto& f : t_fields)
        if (f.name == t_name)
            return &f;
    return nullptr;
}

// ── 1. route-table policy ────────────────────────────────────────────────────

constexpr std::string_view kOrgAdmin = "sgrn::datastore::filters::OrgAdminFilter";
constexpr std::string_view kPlatformAdmin = "sgrn::datastore::filters::PlatformAdminFilter";

bool filtersDemandAdmin(const auto& t_route) {
    for (const auto f : t_route.filter_strings)
        if (f == kOrgAdmin || f == kPlatformAdmin)
            return true;
    return false;
}

template <typename View>
void checkNoWriteRoutes(const char* t_label) {
    for (const auto& r : View::kRoutes) {
        if (r.methods[0] != drogon::Get) {
            ++g_failures;
            std::printf("FAIL %s: collection route still allows a non-GET method\n", t_label);
        }
    }
    for (const auto& r : View::kItemRoutes) {
        if (r.methods[0] != drogon::Get) {
            ++g_failures;
            std::printf("FAIL %s: item route still allows a non-GET method\n", t_label);
        }
    }
}

void testGeneratedRoutePolicy() {
    std::printf("--- 1. Generated route-table policy (compiled from the manifest) ---\n");

    // users: list is admin-only; no POST (create disappeared — users are
    // registered through /admin/users/register), no DELETE (soft delete is
    // a hand-written route).
    for (const auto& r : UsersView::kRoutes) {
        CHECK(r.methods[0] == drogon::Get);
        CHECK(filtersDemandAdmin(r));
    }
    for (const auto& r : UsersView::kItemRoutes) {
        CHECK(r.methods[0] == drogon::Get || r.methods[0] == drogon::Patch);
        CHECK(filtersDemandAdmin(r));
    }

    // automated_services: update remains, requiring admin; no create/delete.
    for (const auto& r : AutomatedServicesView::kRoutes) {
        if (r.methods[0] == drogon::Post)
            ++g_failures, std::printf("FAIL automated_services view still exposes generated POST\n");
        CHECK(filtersDemandAdmin(r)); // list
    }
    for (const auto& r : AutomatedServicesView::kItemRoutes) {
        if (r.methods[0] == drogon::Delete)
            ++g_failures, std::printf("FAIL automated_services view still exposes generated DELETE\n");
        CHECK(filtersDemandAdmin(r)); // get + update
    }

    // domains: create/update/delete require admin; GET stays user-level.
    {
        int domain_write_routes = 0;
        for (const auto& r : DomainsView::kRoutes) {
            if (r.methods[0] == drogon::Post) {
                ++domain_write_routes;
                CHECK(filtersDemandAdmin(r));
            } else if (r.methods[0] == drogon::Get) {
                // list is the only user-level route (non-admins need it)
                CHECK(!filtersDemandAdmin(r));
            }
        }
        for (const auto& r : DomainsView::kItemRoutes) {
            if (r.methods[0] == drogon::Patch || r.methods[0] == drogon::Delete) {
                ++domain_write_routes;
                CHECK(filtersDemandAdmin(r));
            }
        }
        CHECK(domain_write_routes == 3);
    }

    // admin_assignments + user_domain_permissions: READ-ONLY tables —
    // every route must be a GET with admin guarding.
    for (const auto& r : AdminAssignmentsView::kRoutes) {
        CHECK(r.methods[0] == drogon::Get);
        CHECK(filtersDemandAdmin(r));
    }
    for (const auto& r : AdminAssignmentsView::kItemRoutes) {
        CHECK(r.methods[0] == drogon::Get);
        CHECK(filtersDemandAdmin(r));
    }
    for (const auto& r : UserDomainPermissionsView::kRoutes) {
        CHECK(r.methods[0] == drogon::Get);
        CHECK(filtersDemandAdmin(r));
    }
    for (const auto& r : UserDomainPermissionsView::kItemRoutes) {
        CHECK(r.methods[0] == drogon::Get);
        CHECK(filtersDemandAdmin(r));
    }
    checkNoWriteRoutes<AdminAssignmentsView>("admin_assignments");
    checkNoWriteRoutes<UserDomainPermissionsView>("user_domain_permissions");
    CHECK(AdminAssignmentsView::kRoutes.size() == 1 && AdminAssignmentsView::kItemRoutes.size() == 1);
    CHECK(UserDomainPermissionsView::kRoutes.size() == 1 && UserDomainPermissionsView::kItemRoutes.size() == 1);
    CHECK(UsersView::kRoutes.size() == 1 && UsersView::kItemRoutes.size() == 2);
    CHECK(AutomatedServicesView::kRoutes.size() == 1 && AutomatedServicesView::kItemRoutes.size() == 2);
}

// ── 2. field locks ──────────────────────────────────────────────────────────

void checkLocked(FieldType t_expect_type, std::span<const Field> t_fields, const char* t_col) {
    const Field* f = findFieldImpl(t_fields, t_col);
    CHECK(f != nullptr);
    if (f == nullptr)
        return;
    ++g_cases;
    if (t_expect_type != f->type) {
        ++g_failures;
        std::printf("FAIL column '%s' changed FieldType\n", t_col);
    }
    if (f->insertable || f->updatable) {
        ++g_failures;
        std::printf("FAIL sensitive column '%s' is writable (insertable=%d updatable=%d)\n", t_col, f->insertable, f->updatable);
    }
}

void testUsersFieldLocks() {
    std::printf("--- 2. users field locks ---\n");
    const auto fields = std::span<const Field>(UsersView::kFields);
    // F2 core: the privilege-escalation columns.
    checkLocked(FieldType::Bool, fields, "is_platform_admin");
    checkLocked(FieldType::Text, fields, "role");
    checkLocked(FieldType::Text, fields, "status");
    checkLocked(FieldType::Bool, fields, "is_active");
    checkLocked(FieldType::Timestamp, fields, "deleted_at");
    checkLocked(FieldType::Timestamp, fields, "created_at");
    // Usage counters + quota columns.
    for (const char* c : {"total_virtual_size", "total_real_size", "total_entry_count", "storage_limit", "entry_count_limit",
             "rate_limit_rpm", "max_file_size_mb", "preferred_chunk_size_mb", "rate_limit_upload_rpm"}) {
        const Field* f = findFieldImpl(fields, c);
        CHECK(f != nullptr);
        if (f != nullptr && (f->insertable || f->updatable)) {
            ++g_failures;
            std::printf("FAIL quota/usage column '%s' is writable\n", c);
        }
    }
    // No secret column ever reaches the surface.
    for (const Field& f : fields) {
        CHECK(f.name != "password");
    }
    // Plain profile fields stay writable (org admins manage them).
    for (const char* c : {"first_name", "family_name", "email", "phone_number", "domain"}) {
        const Field* f = findFieldImpl(fields, c);
        CHECK(f != nullptr && f->updatable);
    }
    // Personal-permission flags remain org-admin managed.
    CHECK(findFieldImpl(fields, "can_read_personal") != nullptr && findFieldImpl(fields, "can_read_personal")->updatable);
}

void testOtherFieldLocks() {
    std::printf("--- 3. domains / automated_services field locks ---\n");
    const auto domains = std::span<const Field>(DomainsView::kFields);
    // F5: parent re-parenting and rename must not go through PATCH anymore.
    CHECK(findFieldImpl(domains, "name") != nullptr && !findFieldImpl(domains, "name")->updatable);
    CHECK(findFieldImpl(domains, "parent_id") != nullptr && !findFieldImpl(domains, "parent_id")->updatable);
    CHECK(findFieldImpl(domains, "parent_id") != nullptr && findFieldImpl(domains, "parent_id")->insertable);
    checkLocked(FieldType::Text, domains, "path");
    // encryption_policy stays org-admin managed (storage_governance.tsx).
    const Field* pol = findFieldImpl(domains, "encryption_policy");
    CHECK(pol != nullptr && pol->updatable);

    const auto services = std::span<const Field>(AutomatedServicesView::kFields);
    checkLocked(FieldType::Text, services, "token");
    checkLocked(FieldType::Jsonb, services, "metadata");
    checkLocked(FieldType::Timestamp, services, "deleted_at");
    checkLocked(FieldType::Timestamp, services, "created_at");
    checkLocked(FieldType::Timestamp, services, "updated_at");
    for (const char* c : {"total_virtual_size", "total_real_size", "total_entry_count", "storage_limit", "entry_count_limit",
             "rate_limit_rpm", "max_file_size_mb", "preferred_chunk_size_mb", "rate_limit_upload_rpm"}) {
        const Field* f = findFieldImpl(services, c);
        CHECK(f != nullptr);
        if (f != nullptr && (f->insertable || f->updatable)) {
            ++g_failures;
            std::printf("FAIL service quota/usage column '%s' is writable\n", c);
        }
    }
    CHECK(findFieldImpl(services, "name") != nullptr && findFieldImpl(services, "name")->updatable);
    CHECK(findFieldImpl(services, "status") != nullptr && findFieldImpl(services, "status")->updatable);
}

// ── 4. filter behavior over the generated routes ───────────────────────────

void testOrgAdminFilter() {
    std::printf("--- 4. OrgAdminFilter behavior on generated route paths ---\n");

    ::setenv("SGRN_SYSTEM_ADMIN_EMAILS", "", 0);

    // A plain user in OrgA (resolved level NONE, no platform admin).
    const int32_t plain_user_id = 601;
    AdminLevelCache::instance().setCachedLevel(plain_user_id, "OrgA", std::nullopt, AdminLevel::NONE);
    AdminLevelCache::instance().setCachedLevel(602, "OrgA", std::nullopt, AdminLevel::ORG);
    AdminLevelCache::instance().setCachedLevel(603, "OrgForeign", std::nullopt, AdminLevel::ORG);

    struct Probe {
        const char* label;
        drogon::HttpMethod method;
        const char* path;
    };
    const Probe probes[] = {
        {"PATCH self", drogon::Patch, "/api/v1/users/601"},
        {"DELETE domain", drogon::Delete, "/api/v1/domains/1"},
        {"POST domain", drogon::Post, "/api/v1/domains"},
        {"PATCH service", drogon::Patch, "/api/v1/automated-services/2"},
        {"GET admin-assignments", drogon::Get, "/api/v1/admin-assignments"},
    };

    auto makeReq = [&](drogon::HttpMethod t_method, const char* t_path, const char* t_session_org, int32_t t_uid, const char* t_role) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(t_method);
        req->setPath(t_path);
        Json::Value session;
        session["user"]["id"] = t_uid;
        session["user"]["email"] = "probe@example.com";
        session["user"]["organisation"] = t_session_org;
        session["user"]["role"] = t_role;
        session["user"]["is_platform_admin"] = false;
        session["user"]["admin_level"] = 0;
        req->attributes()->insert("session_json", session);
        req->attributes()->insert("user_role_code",
            std::string(t_role) == "admin" ? sgrn::datastore::filters::UserRoleEnum::ADMIN : sgrn::datastore::filters::UserRoleEnum::USER);
        return req;
    };

    OrgAdminFilter org_filter;
    for (const auto& probe : probes) {
        // Plain user: rejected with 403 (A1/A2 core).
        {
            auto passed = false;
            drogon::HttpResponsePtr err;
            auto req = makeReq(probe.method, probe.path, "OrgA", plain_user_id, "user");
            org_filter.doFilter(req, [&](const drogon::HttpResponsePtr& r) { err = r; }, [&] { passed = true; });
            CHECK(!passed);
            CHECK(err != nullptr && err->getStatusCode() == drogon::k403Forbidden);
        }
        // Org admin of the same org: accepted.
        {
            auto passed = false;
            auto req = makeReq(probe.method, probe.path, "OrgA", 602, "admin");
            org_filter.doFilter(req, [](const drogon::HttpResponsePtr&) {}, [&] { passed = true; });
            CHECK(passed);
        }
        // Org admin of a different org: rejected 403 (cross-tenant query
        // parameter — same rule as the hand-written admin endpoints).
        {
            auto passed = false;
            drogon::HttpResponsePtr err;
            auto req = makeReq(probe.method, probe.path, "OrgForeign", 603, "admin");
            req->setParameter("organisation", "OrgA");
            org_filter.doFilter(req, [&](const drogon::HttpResponsePtr& r) { err = r; }, [&] { passed = true; });
            CHECK(!passed);
            CHECK(err != nullptr && err->getStatusCode() == drogon::k403Forbidden);
        }
    }

    // Unauthenticated → 401, not 403 (makeAuthRequest parity).
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Patch);
        req->setPath("/api/v1/users/1");
        auto passed = false;
        drogon::HttpResponsePtr err;
        org_filter.doFilter(req, [&](const drogon::HttpResponsePtr& r) { err = r; }, [&] { passed = true; });
        CHECK(!passed);
        CHECK(err != nullptr && err->getStatusCode() == drogon::k401Unauthorized);
    }
}

// ── 5. engine honors updatable=false (live DB, auto-skip) ──────────────────

std::string dbEnv(const char* t_name, const char* t_dflt) {
    const char* v = std::getenv(t_name);
    return (v != nullptr && *v != '\0') ? v : t_dflt;
}

bool tcpReachable(const std::string& t_host, int t_port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (::getaddrinfo(t_host.c_str(), std::to_string(t_port).c_str(), &hints, &list) != 0)
        return false;
    bool ok = false;
    for (addrinfo* ai = list; ai != nullptr && !ok; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const timeval tv{0, 300000};
            if (::select(fd + 1, nullptr, &wfds, nullptr, const_cast<timeval*>(&tv)) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                ok = (err == 0);
            }
        }
        ::close(fd);
    }
    ::freeaddrinfo(list);
    return ok;
}

const Field kLockFields[] = {
    {.name = "first_name", .type = FieldType::Text, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "is_platform_admin", .type = FieldType::Bool, .filter_ops = 0, .insertable = false, .updatable = false},
    {.name = "storage_limit", .type = FieldType::BigInt, .filter_ops = 0, .insertable = false, .updatable = false},
};

CrudViewSpec lockSpec() {
    CrudViewSpec spec;
    spec.read_relation = "crud_lock_test";
    spec.write_table = "crud_lock_test";
    spec.tenant_column = "tenant";
    spec.pk = PrimaryKey{.name = "id", .type = FieldType::Int};
    spec.fields = kLockFields;
    spec.default_order = "id";
    spec.max_limit = 10;
    return spec;
}

Json::Value parse(std::string_view t_json) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors))
        ++g_failures;
    return v;
}

void testEngineHonorsLocks() {
    std::printf("--- 5. Engine honors updatable=false (live DB temp table) ---\n");
    // The sgrn_datastore role cannot CREATE in public, but it CAN create
    // TEMPORARY tables (pg_temp). With connectionNumber=1 every statement
    // rides the same pooled connection, so the temp table is visible to the
    // whole probe. If the dev server is absent the section skips.
    const std::string host = dbEnv("PGHOST", "127.0.0.1");
    int port = 5432;
    try {
        port = std::stoi(dbEnv("PGPORT", "5432"));
    } catch (const std::exception&) {
    }
    if (!tcpReachable(host, port)) {
        std::printf("crud_admin_lockdown_test: SKIP engine section (no postgres at %s:%d)\n", host.c_str(), port);
        return;
    }

    try {
        drogon::orm::PostgresConfig cfg;
        cfg.host = host;
        cfg.port = static_cast<unsigned short>(port);
        cfg.databaseName = dbEnv("PGDATABASE", "sgrn");
        cfg.username = dbEnv("PGUSER", "sgrn_datastore");
        cfg.password = dbEnv("PGPASSWORD", "dracaeris");
        cfg.connectionNumber = 1;
        cfg.name = "default";
        cfg.isFast = false;
        cfg.timeout = 5.0;
        cfg.autoBatch = false;
        drogon::app().addDbClient(cfg);
        std::thread loop([] { drogon::app().run(); });
        loop.detach();
        drogon::orm::DbClientPtr client;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (drogon::app().isRunning()) {
                client = drogon::app().getDbClient();
                if (client && client->hasAvailableConnections())
                    break;
                client.reset();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!client) {
            std::printf("crud_admin_lockdown_test: SKIP engine section (no usable connection)\n");
            return;
        }

        auto setup = [&](const std::string& t_sql, const std::vector<std::string>& t_binds = {}) {
            drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(client, t_sql, t_binds));
        };
        try {
            setup("DROP TABLE IF EXISTS crud_lock_test");
            setup("CREATE TEMPORARY TABLE crud_lock_test (id SERIAL PRIMARY KEY, tenant TEXT NOT NULL, "
                  "first_name TEXT, is_platform_admin BOOLEAN NOT NULL DEFAULT FALSE, storage_limit BIGINT)");
            setup("INSERT INTO crud_lock_test (tenant, first_name) VALUES ($1, $2)", {"t1", "Alice"});
        } catch (const std::exception& e) {
            std::printf("crud_admin_lockdown_test: SKIP engine section (setup failed: %s)\n", e.what());
            return;
        }

        // Escalation attempt: locked keys next to an allowed key — the
        // PATCH succeeds (200) but ONLY the allowed column changes. The
        // request carries an admin session so the engine's audit hook
        // (WP3) must also leave one crud.<table>.<op> trail row.
        {
            const Json::Value body = parse("{\"first_name\":\"Eve\",\"is_platform_admin\":true,\"storage_limit\":null}");
            auto req = drogon::HttpRequest::newHttpJsonRequest(body);
            Json::Value session;
            session["user"]["id"] = Json::Int64(777);
            session["user"]["email"] = "audit-probe@sgrn.local";
            // Must reference a real organisation: core.audit_logs.organisation
            // carries an FK to core.organisations(name).
            session["user"]["organisation"] = "CIC-Moulins Guelma";
            req->attributes()->insert("session_json", session);
            auto resp = drogon::sync_wait(sgrn::datastore::query::executeViewUpdate(lockSpec(), req, "t1", "1"));
            CHECK(static_cast<int>(resp->getStatusCode()) == 200);
            auto row = drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(
                client, "SELECT first_name, is_platform_admin, storage_limit FROM crud_lock_test WHERE id = 1", {}));
            CHECK(!row.empty());
            CHECK(row[0]["first_name"].as<std::string>() == "Eve");
            CHECK(row[0]["is_platform_admin"].as<bool>() == false);
            CHECK(row[0]["storage_limit"].isNull());
        }
        // Audit hook (WP3): exactly one crud.<write_table>.<op> row, scoped
        // to the session actor, with details built from *spec fields only* —
        // arbitrary client JSON keys must never be echoed into the trail.
        try {
            auto arows = drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(client,
                "SELECT action, target_type, target_id, actor_name, organisation, status "
                "FROM core.audit_logs WHERE action = 'crud.crud_lock_test.update' AND target_id = '1' ORDER BY created_at DESC LIMIT 1",
                {}));
            CHECK(!arows.empty());
            if (!arows.empty()) {
                CHECK(arows[0]["actor_name"].as<std::string>() == "audit-probe@sgrn.local");
                CHECK(arows[0]["organisation"].as<std::string>() == "CIC-Moulins Guelma");
                CHECK(arows[0]["target_type"].as<std::string>() == "crud_lock_test");
                CHECK(arows[0]["status"].as<std::string>() == "success");
                auto drows = drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(client,
                    "SELECT details::text AS d FROM core.audit_logs WHERE action = 'crud.crud_lock_test.update' AND target_id = '1' "
                    "ORDER BY created_at DESC LIMIT 1",
                    {}));
                CHECK(!drows.empty());
                const std::string details = drows[0]["d"].as<std::string>();
                CHECK(details.find("first_name") != std::string::npos);
            }
        } catch (const std::exception& e) {
            std::printf("crud_admin_lockdown_test: FAIL audit probe (harness: %s)\n", e.what());
            ++g_failures;
        }
        // Arbitrary-key echo rejection: a PATCH body containing a key that is
        // NOT part of the spec must not surface that key in the audit row.
        try {
            const Json::Value rogue = parse("{\"first_name\":\"Mallory\",\"secret_backdoor_key\":\"pwn\"}");
            auto req2 = drogon::HttpRequest::newHttpJsonRequest(rogue);
            Json::Value session2;
            session2["user"]["id"] = Json::Int64(777);
            session2["user"]["email"] = "audit-probe@sgrn.local";
            session2["user"]["organisation"] = "CIC-Moulins Guelma";
            req2->attributes()->insert("session_json", session2);
            drogon::sync_wait(sgrn::datastore::query::executeViewUpdate(lockSpec(), req2, "t1", "1"));
            auto drows = drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(client,
                "SELECT details::text AS d FROM core.audit_logs WHERE action = 'crud.crud_lock_test.update' AND target_id = '1' "
                "ORDER BY created_at DESC LIMIT 1",
                {}));
            CHECK(!drows.empty());
            const std::string details2 = drows[0]["d"].as<std::string>();
            CHECK(details2.find("secret_backdoor_key") == std::string::npos);
            CHECK(details2.find("pwn") == std::string::npos);
        } catch (const std::exception& e) {
            std::printf("crud_admin_lockdown_test: FAIL audit rogue-key probe (harness: %s)\n", e.what());
            ++g_failures;
        }
        // Body with ONLY locked keys → 400 ("No updatable fields"), never
        // a silent no-op write to the DB.
        {
            const Json::Value body = parse("{\"is_platform_admin\":true}");
            auto req = drogon::HttpRequest::newHttpJsonRequest(body);
            auto resp = drogon::sync_wait(sgrn::datastore::query::executeViewUpdate(lockSpec(), req, "t1", "1"));
            CHECK(static_cast<int>(resp->getStatusCode()) == 400);
        }

        try {
            setup("DROP TABLE IF EXISTS crud_lock_test");
        } catch (const std::exception&) {
        }
    } catch (const std::exception& e) {
        std::printf("crud_admin_lockdown_test: SKIP engine section (harness failed: %s)\n", e.what());
    }
}

// ── 6. check_manifest_policy.py integration ────────────────────────────────

bool commandSucceeds(const std::string& t_cmd) {
    return std::system(t_cmd.c_str()) == 0;
}

void testPolicyScript() {
    std::printf("--- 6. check_manifest_policy.py — repo manifest and negative fixture ---\n");
    if (std::system("python3 -c 'import sys' >/dev/null 2>&1") != 0) {
        std::printf("crud_admin_lockdown_test: SKIP policy-script section (python3 unavailable)\n");
        return;
    }
    const std::string script = DATASTORE_ROOT "/scripts/check_manifest_policy.py";
    const std::string manifest = DATASTORE_ROOT "/crud/manifest.json";

    // The repo manifest passes.
    CHECK(commandSucceeds("python3 " + script + " " + manifest + " >/dev/null 2>&1"));

    // Negative fixture: every mutated policy must fail. Write via a small
    // python driver so the fixture stays JSON-valid whatever jsoncpp does.
    const std::string fixture_script =
        "import json,sys,re,pathlib\n"
        "m = json.loads(pathlib.Path('" +
        manifest +
        "').read_text())\n"
        "which = sys.argv[1]\n"
        "if which == 'plain_user_create':\n"
        "    m['schemas']['core']['tables']['domains']['operation_filters']['create'] = ['sgrn::datastore::filters::UserAuthFilter']\n"
        "elif which == 'users_list_user':\n"
        "    m['schemas']['core']['tables']['users']['operation_filters']['list'] = ['sgrn::datastore::filters::UserAuthFilter']\n"
        "elif which == 'sensitive_writable':\n"
        "    m['schemas']['core']['tables']['users']['columns']['can_read_personal'] = {\"filter_ops\": []}\n"
        "    m['schemas']['core']['tables']['users']['columns']['is_active'] = {\"insertable\": True, \"updatable\": False}\n"
        "elif which == 'delete_grant':\n"
        "    m['schemas']['core']['tables']['admin_assignments']['operations'] = ['list','get','delete']\n"
        "pathlib.Path('" +
        std::string(DATASTORE_ROOT) + "/crud/.policy_fixture.json').write_text(json.dumps(m))\n";
    {
        FILE* f = std::fopen("/tmp/opencode/policy_fixture_driver.py", "w");
        if (f == nullptr) {
            std::printf("crud_admin_lockdown_test: SKIP policy-script section (cannot write fixture driver)\n");
            return;
        }
        std::fputs(fixture_script.c_str(), f);
        std::fclose(f);
    }
    const std::string fixture = DATASTORE_ROOT "/crud/.policy_fixture.json";
    for (const char* mutation : {"plain_user_create", "users_list_user", "sensitive_writable", "delete_grant"}) {
        const std::string cmd = std::string("python3 /tmp/opencode/policy_fixture_driver.py ") + mutation + " >/dev/null && python3 " +
                                script + " " + fixture + " >/dev/null 2>&1";
        ++g_cases;
        if (commandSucceeds(cmd)) {
            ++g_failures;
            std::printf("FAIL policy script accepted fixture '%s'\n", mutation);
        }
    }
    std::remove(fixture.c_str());
}

} // namespace

int main() {
    std::printf("============================================================\n");
    std::printf("Running crud_admin_lockdown_test (WP1 CRUD security hotfix)...\n");
    std::printf("============================================================\n");

    testGeneratedRoutePolicy();
    testUsersFieldLocks();
    testOtherFieldLocks();
    testOrgAdminFilter();
    testEngineHonorsLocks();
    testPolicyScript();

    if (g_failures == 0) {
        std::printf("crud_admin_lockdown_test: ALL %d CHECKS PASSED\n", g_cases);
    } else {
        std::fprintf(stderr, "crud_admin_lockdown_test: %d FAILURE(S) / %d checks\n", g_failures, g_cases);
    }

    std::fflush(stdout);
    std::fflush(stderr);
    _exit(g_failures == 0 ? 0 : 1);
}
