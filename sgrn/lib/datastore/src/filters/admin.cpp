#include <sgrn/datastore/filters/admin.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/debug.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace sgrn::datastore::filters
{

AdminLevelCache& AdminLevelCache::instance() {
    static AdminLevelCache cache;
    return cache;
}

drogon::Task<AdminLevel> AdminLevelCache::resolveAdminLevel(
    int32_t t_user_id, const std::string& t_org, std::optional<int32_t> t_domain_id, const std::string& t_email) {
    const int32_t domain_key = t_domain_id.value_or(-1);
    const CacheKey key{t_user_id, t_org, domain_key};

    // 1. Fast path: check in-process cache
    {
        std::shared_lock lock(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            if (std::chrono::steady_clock::now() < it->second.expiry) {
                co_return it->second.level;
            }
        }
    }

    // 2. Query core.admin_level() in database
    AdminLevel resolved_level = AdminLevel::NONE;
    bool db_queried = false;

    try {
        auto db = drogon::app().getDbClient();
        if (db) {
            std::optional<drogon::orm::Result> res_opt;
            if (t_domain_id.has_value()) {
                res_opt = co_await db->execSqlCoro("SELECT core.admin_level($1::int, $2::text, $3::int)", t_user_id, t_org, *t_domain_id);
            } else {
                res_opt = co_await db->execSqlCoro("SELECT core.admin_level($1::int, $2::text, NULL)", t_user_id, t_org);
            }

            if (res_opt && !res_opt->empty() && !(*res_opt)[0][0].isNull()) {
                const int val = (*res_opt)[0][0].as<int>();
                resolved_level = static_cast<AdminLevel>(val);
                db_queried = true;
            }
        }
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("AdminLevelCache: core.admin_level() failed: {}", e.what());
    }

    // Bootstrap admins are exact, case-insensitive matches. A DB failure is
    // deliberately not cached, so a transient outage cannot lock admins out.
    if (!db_queried) {
        if (sgrn::datastore::auth::emailIsConfiguredPlatformAdmin(t_email)) resolved_level = AdminLevel::PLATFORM;
        co_return resolved_level;
    }
    if (resolved_level != AdminLevel::PLATFORM && sgrn::datastore::auth::emailIsConfiguredPlatformAdmin(t_email)) {
        resolved_level = AdminLevel::PLATFORM;
    }

    // 4. Update cache
    {
        std::unique_lock lock(m_mutex);
        m_cache[key] = CacheEntry{resolved_level, std::chrono::steady_clock::now() + m_ttl};
    }

    co_return resolved_level;
}

void AdminLevelCache::invalidateUser(int32_t t_user_id) {
    std::unique_lock lock(m_mutex);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it->first.user_id == t_user_id) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void AdminLevelCache::invalidateAll() {
    std::unique_lock lock(m_mutex);
    m_cache.clear();
}

// ── Standard Resolvers ────────────────────────────────────────────────────────

TargetResource resolveSessionOrg(const drogon::HttpRequestPtr& req) {
    TargetResource target;
    target.organisation = sgrn::datastore::getSessionOrganisation(req);
    target.domain_id = std::nullopt;
    return target;
}

TargetResource resolveDomainTarget(const drogon::HttpRequestPtr& req) {
    TargetResource target;
    target.organisation = sgrn::datastore::getSessionOrganisation(req);

    // Look for domain_id in query params
    std::string q_domain = req->getParameter("domain_id");
    if (q_domain.empty()) {
        q_domain = req->getParameter("domain");
    }
    if (!q_domain.empty()) {
        try {
            target.domain_id = std::stoi(q_domain);
        } catch (...) {
        }
    }
    return target;
}

TargetResource resolveOrgTarget(const drogon::HttpRequestPtr& req) {
    TargetResource target;
    target.organisation = sgrn::datastore::getSessionOrganisation(req);
    target.domain_id = std::nullopt;
    return target;
}

// ── ScopeFilter Implementation ────────────────────────────────────────────────

ScopeFilter::ScopeFilter(AdminLevel t_required_level, TargetResourceResolver t_resolver)
    : m_required_level(t_required_level)
    , m_resolver(std::move(t_resolver)) {
}

void ScopeFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    // 1. Verify authentication context
    if (!tsp_req->attributes()->find("session_json")) {
        respondWithError("Unauthorized: Authentication Required", drogon::k401Unauthorized, std::move(tsp_filter_callback));
        return;
    }

    const Json::Value& session_json = tsp_req->attributes()->get<Json::Value>("session_json");
    if (!session_json.isMember("user")) {
        respondWithError("Unauthorized: Incomplete Session Claims", drogon::k401Unauthorized, std::move(tsp_filter_callback));
        return;
    }

    const Json::Value& user_obj = session_json["user"];
    const int32_t caller_user_id = user_obj.isMember("id") ? user_obj["id"].asInt() : 0;
    const std::string caller_email = user_obj.isMember("email") ? user_obj["email"].asString() : "";
    const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);

    // 2. Check if platform admin via configuration/email
    // The request attribute is populated by this filter, so before the async
    // DB lookup only the bootstrap list can be known locally.
    const bool is_platform_caller = sgrn::datastore::auth::emailIsConfiguredPlatformAdmin(caller_email);

    // 3. Resolve target resource
    TargetResource target_res;
    if (m_resolver) {
        target_res = m_resolver(tsp_req);
    } else {
        target_res = resolveSessionOrg(tsp_req);
    }

    // 4. Strict tenant boundary rule:
    // "For non-platform callers the organisation is ALWAYS the session organisation."
    if (!is_platform_caller) {
        const std::string q_org = tsp_req->getParameter("organisation");
        if (!q_org.empty() && q_org != session_org) {
            SGRN_WARN_LOG("ScopeFilter: Rejected cross-organisation query param '{}' for non-platform user '{}'", q_org, caller_email);
            respondWithError("Forbidden: Cross-organisation targeting disallowed", drogon::k403Forbidden, std::move(tsp_filter_callback));
            return;
        }

        // Also check JSON body if present and contains "organisation"
        auto p_body = tsp_req->getJsonObject();
        if (p_body && p_body->isMember("organisation")) {
            const std::string body_org = (*p_body)["organisation"].asString();
            if (!body_org.empty() && body_org != session_org) {
                SGRN_WARN_LOG(
                    "ScopeFilter: Rejected cross-organisation body payload '{}' for non-platform user '{}'", body_org, caller_email);
                respondWithError(
                    "Forbidden: Cross-organisation targeting disallowed", drogon::k403Forbidden, std::move(tsp_filter_callback));
                return;
            }
        }

        // Always lock target to session organisation
        target_res.organisation = session_org;
    }

    // If target organisation is empty (e.g. global platform endpoint), fallback to session org
    if (target_res.organisation.empty()) {
        target_res.organisation = session_org;
    }

    // Fast-path: if caller is verified platform admin and route requires up to PLATFORM
    if (is_platform_caller) {
        tsp_req->attributes()->insert("caller_admin_level", AdminLevel::PLATFORM);
        tsp_req->attributes()->insert("admin_level", AdminLevel::PLATFORM);
        tsp_req->attributes()->insert("is_platform_admin", true);
        tsp_req->attributes()->insert("target_organisation", target_res.organisation);
        if (target_res.domain_id.has_value()) {
            tsp_req->attributes()->insert("target_domain_id", *target_res.domain_id);
        }
        t_filter_chain_callback();
        return;
    }

    // Resolve DB-backed platform/org/domain assignments asynchronously.
    drogon::async_run(
        [tsp_req, tsp_filter_callback = std::move(tsp_filter_callback), t_filter_chain_callback = std::move(t_filter_chain_callback),
            caller_user_id, caller_email, target_res, req_level = m_required_level]() mutable -> drogon::Task<void> {
            AdminLevel caller_level = co_await AdminLevelCache::instance().resolveAdminLevel(
                caller_user_id, target_res.organisation, target_res.domain_id, caller_email);

            if (static_cast<uint8_t>(caller_level) < static_cast<uint8_t>(req_level)) {
                SGRN_WARN_LOG("ScopeFilter: Denied access for user '{}' (level={}, required={}) on org '{}'", caller_email,
                    adminLevelToString(caller_level), adminLevelToString(req_level), target_res.organisation);
                respondWithError(fmt::format("Forbidden: Insufficient Administrative Privilege (requires {}, caller is {})",
                                     adminLevelToString(req_level), adminLevelToString(caller_level)),
                    drogon::k403Forbidden, tsp_filter_callback);
                co_return;
            }

            // Access granted: hydrate attributes
            tsp_req->attributes()->insert("caller_admin_level", caller_level);
            tsp_req->attributes()->insert("admin_level", caller_level);
            tsp_req->attributes()->insert("is_platform_admin", caller_level == AdminLevel::PLATFORM);
            tsp_req->attributes()->insert("target_organisation", target_res.organisation);
            if (target_res.domain_id.has_value()) {
                tsp_req->attributes()->insert("target_domain_id", *target_res.domain_id);
            }

            t_filter_chain_callback();
        });
}

std::shared_ptr<ScopeFilter> requireLevel(AdminLevel level, TargetResourceResolver resolver) {
    return std::make_shared<ScopeFilter>(level, std::move(resolver));
}

// ── Concrete Filter Implementations ───────────────────────────────────────────

void PlatformAdminFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    static ScopeFilter filter(AdminLevel::PLATFORM, resolveSessionOrg);
    filter.doFilter(tsp_req, std::move(tsp_filter_callback), std::move(t_filter_chain_callback));
}

void OrgAdminFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    static ScopeFilter filter(AdminLevel::ORG, resolveOrgTarget);
    filter.doFilter(tsp_req, std::move(tsp_filter_callback), std::move(t_filter_chain_callback));
}

void DomainAdminFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    static ScopeFilter filter(AdminLevel::DOMAIN, resolveDomainTarget);
    filter.doFilter(tsp_req, std::move(tsp_filter_callback), std::move(t_filter_chain_callback));
}

void AdminFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    static ScopeFilter filter(AdminLevel::ORG, resolveOrgTarget);
    filter.doFilter(tsp_req, std::move(tsp_filter_callback), std::move(t_filter_chain_callback));
}

} // namespace sgrn::datastore::filters
