/*sgrn/lib/datastore/tests/cross-org-auth-test.cpp*/
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/handlers/storage-admin.hpp>
#include <sgrn/datastore/utils/helpers.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace drogon;
int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

HttpRequestPtr makeRequest(HttpMethod t_method, const std::string& t_path, const std::string& t_email, const std::string& t_org,
    int t_user_id = 1, const Json::Value& t_body = Json::Value::null) {
    HttpRequestPtr req;
    if (!t_body.isNull()) {
        req = HttpRequest::newHttpJsonRequest(t_body);
    } else {
        req = HttpRequest::newHttpRequest();
    }
    req->setMethod(t_method);
    req->setPath(t_path);

    Json::Value session;
    session["user"]["id"] = t_user_id;
    session["user"]["email"] = t_email;
    session["user"]["organisation"] = t_org;
    session["user"]["role"] = "admin";

    req->attributes()->insert("session_json", session);
    req->attributes()->insert("user_role_code", sgrn::datastore::filters::UserRoleEnum::ADMIN);
    return req;
}

void testPlatformAdminHelper() {
    std::printf("Testing isPlatformAdmin helper...\n");

    // Default tenant admin: not a platform admin
    auto req_a = makeRequest(Get, "/api/v1/admin/users", "admin@orga.com", "OrgA");
    CHECK(!sgrn::datastore::isPlatformAdmin(req_a));
    CHECK(sgrn::datastore::getSessionOrganisation(req_a) == "OrgA");

    auto req_b = makeRequest(Get, "/api/v1/admin/users", "admin@orgb.com", "OrgB");
    CHECK(!sgrn::datastore::isPlatformAdmin(req_b));
    CHECK(sgrn::datastore::getSessionOrganisation(req_b) == "OrgB");

    // Environment variable configuration for system admin
    ::setenv("SGRN_SYSTEM_ADMIN_EMAILS", "sysadmin@sgrn.local,super@platform.gov", 1);
    auto req_sys = makeRequest(Get, "/api/v1/admin/system/config", "sysadmin@sgrn.local", "PlatformOrg");
    CHECK(sgrn::datastore::isPlatformAdmin(req_sys));

    auto req_super = makeRequest(Get, "/api/v1/admin/system/config", "super@platform.gov", "PlatformOrg");
    CHECK(sgrn::datastore::isPlatformAdmin(req_super));

    auto req_other = makeRequest(Get, "/api/v1/admin/system/config", "other@platform.gov", "PlatformOrg");
    CHECK(!sgrn::datastore::isPlatformAdmin(req_other));
}

void testStorageAdminPlatformGating() {
    std::printf("Testing /admin/system/config and /admin/storage/orphans/purge gating...\n");

    sgrn::datastore::handlers::storage_admin::StorageAdminHandler handler;

    // Tenant admin attempting GET /admin/system/config -> 403 Forbidden
    auto req_get_cfg = makeRequest(Get, "/api/v1/admin/system/config", "admin@orga.com", "OrgA");
    auto resp_get_cfg = sync_wait(handler.handleGetSystemConfig(req_get_cfg));
    CHECK(resp_get_cfg->getStatusCode() == k403Forbidden);

    // Tenant admin attempting PUT /admin/system/config -> 403 Forbidden
    Json::Value put_cfg_json;
    put_cfg_json["values"]["app.log_level"] = "DEBUG";
    auto req_put_cfg = makeRequest(Put, "/api/v1/admin/system/config", "admin@orga.com", "OrgA", 1, put_cfg_json);
    auto resp_put_cfg = sync_wait(handler.handleUpdateSystemConfig(req_put_cfg));
    CHECK(resp_put_cfg->getStatusCode() == k403Forbidden);

    // Tenant admin attempting POST /admin/storage/orphans/purge -> 403 Forbidden
    Json::Value purge_json;
    purge_json["bucket"] = "sgrn-uploads";
    purge_json["dry_run"] = true;
    auto req_purge_orphans = makeRequest(Post, "/api/v1/admin/storage/orphans/purge", "admin@orga.com", "OrgA", 1, purge_json);
    auto resp_purge_orphans = sync_wait(handler.handlePurgeOrphans(req_purge_orphans));
    CHECK(resp_purge_orphans->getStatusCode() == k403Forbidden);
}

void testAuditLogsPurgeGating() {
    std::printf("Testing DELETE /admin/audit-logs gating and retention floor...\n");

    sgrn::datastore::handlers::admin::AdminApiHandler handler;

    // Tenant admin attempting DELETE /admin/audit-logs -> 403 Forbidden
    auto req_tenant_purge = makeRequest(Delete, "/api/v1/admin/audit-logs", "admin@orga.com", "OrgA");
    req_tenant_purge->setParameter("older_than_days", "180");
    auto resp_tenant = sync_wait(handler.handlePurgeAuditLogs(req_tenant_purge));
    CHECK(resp_tenant->getStatusCode() == k403Forbidden);

    // Platform admin attempting purge with retention < 90 days -> 400 Bad Request
    auto req_plat_short = makeRequest(Delete, "/api/v1/admin/audit-logs", "sysadmin@sgrn.local", "PlatformOrg");
    req_plat_short->setParameter("older_than_days", "30");
    auto resp_plat_short = sync_wait(handler.handlePurgeAuditLogs(req_plat_short));
    CHECK(resp_plat_short->getStatusCode() == k400BadRequest);
}

void testPermissionsCrossOrgFilterGating() {
    std::printf("Testing GET /admin/permissions foreign organisation parameter rejection...\n");

    sgrn::datastore::handlers::admin::AdminApiHandler handler;

    // Admin A passes organisation=OrgB -> rejected with 403 Forbidden
    auto req_cross_org = makeRequest(Get, "/api/v1/admin/permissions", "admin@orga.com", "OrgA");
    req_cross_org->setParameter("organisation", "OrgB");
    auto resp_cross = sync_wait(handler.handleListPermissions(req_cross_org));
    CHECK(resp_cross->getStatusCode() == k403Forbidden);
}

void testUserRegistrationCrossOrgGating() {
    std::printf("Testing POST /admin/users/register cross-org body rejection...\n");

    sgrn::datastore::handlers::admin::AdminApiHandler handler;

    // Admin A sends registration with organisation=OrgB in payload
    Json::Value reg_json;
    reg_json["first_name"] = "Alice";
    reg_json["family_name"] = "Smith";
    reg_json["email"] = "alice@orgb.com";
    reg_json["password"] = "SecretPassword123!";
    reg_json["organisation"] = "OrgB";
    auto req_reg = makeRequest(Post, "/api/v1/admin/users/register", "admin@orga.com", "OrgA", 1, reg_json);

    auto resp_reg = sync_wait(handler.handleRegisterUser(req_reg));
    CHECK(resp_reg->getStatusCode() == k403Forbidden);
}

void testGetUsersCrossOrgParamGating() {
    std::printf("Testing GET /admin/users foreign organisation parameter rejection...\n");

    sgrn::datastore::handlers::admin::AdminApiHandler handler;

    // Admin A passes ?organisation=OrgB -> rejected with 403 Forbidden
    auto req_users = makeRequest(Get, "/api/v1/admin/users", "admin@orga.com", "OrgA");
    req_users->setParameter("organisation", "OrgB");

    auto resp_users = sync_wait(handler.handleGetUsers(req_users));
    CHECK(resp_users->getStatusCode() == k403Forbidden);
}

void testStorageAdminUpdateQuotaCrossOrgGating() {
    std::printf("Testing PUT /admin/quotas foreign organisation quota tampering rejection...\n");

    sgrn::datastore::handlers::storage_admin::StorageAdminHandler handler;

    // Admin A attempts to update OrgB quota
    Json::Value q_json;
    q_json["kind"] = "organisation";
    q_json["name"] = "OrgB";
    q_json["storage_limit_bytes"] = 1048576;
    auto req_quota = makeRequest(Put, "/api/v1/admin/quotas", "admin@orga.com", "OrgA", 1, q_json);

    auto resp_quota = sync_wait(handler.handleUpdateQuota(req_quota));
    CHECK(resp_quota->getStatusCode() == k403Forbidden);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::printf("============================================================\n");
    std::printf("Running cross_org_auth_test...\n");
    std::printf("============================================================\n");

    testPlatformAdminHelper();
    testStorageAdminPlatformGating();
    testAuditLogsPurgeGating();
    testPermissionsCrossOrgFilterGating();
    testUserRegistrationCrossOrgGating();
    testGetUsersCrossOrgParamGating();
    testStorageAdminUpdateQuotaCrossOrgGating();

    if (g_failures == 0) {
        std::printf("cross_org_auth_test: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stderr, "cross_org_auth_test: %d FAILURE(S)\n", g_failures);
    }

    // NOTE: _exit(), not return. Handlers touch drogon::app().getDbClient(),
    // which instantiates the framework singleton; its static teardown segfaults
    // when the event loop never ran. Flush first so no output is lost, then
    // bypass static destructors with the exit code (same as crud-binds-test.cpp).
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(g_failures == 0 ? 0 : 1);
}
