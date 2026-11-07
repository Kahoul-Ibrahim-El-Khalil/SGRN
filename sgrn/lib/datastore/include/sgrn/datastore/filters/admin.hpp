#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/filters/types.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace sgrn::datastore::filters
{

// In-process TTL cache for core.admin_level() queries.
// Fails closed to AdminLevel::NONE on any DB disconnection or query error.
class AdminLevelCache {
public:
    static AdminLevelCache& instance();

    drogon::Task<AdminLevel> resolveAdminLevel(
        int32_t t_user_id, const std::string& t_org, std::optional<int32_t> t_domain_id, const std::string& t_email = "");

    void invalidateUser(int32_t t_user_id);
    void invalidateAll();

    void setCachedLevel(int32_t t_user_id, const std::string& t_org, std::optional<int32_t> t_domain_id, AdminLevel t_level) {
        std::unique_lock lock(m_mutex);
        CacheKey key{t_user_id, t_org, t_domain_id.value_or(-1)};
        m_cache[key] = CacheEntry{t_level, std::chrono::steady_clock::now() + m_ttl};
    }

    void setTtl(std::chrono::seconds t_ttl) {
        std::unique_lock lock(m_mutex);
        m_ttl = t_ttl;
    }

private:
    AdminLevelCache() = default;

    struct CacheKey {
        int32_t user_id;
        std::string org;
        int32_t domain_id; // -1 represents NULL (organisation level)

        bool operator==(const CacheKey& other) const {
            return user_id == other.user_id && org == other.org && domain_id == other.domain_id;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            std::size_t h1 = std::hash<int32_t>{}(k.user_id);
            std::size_t h2 = std::hash<std::string>{}(k.org);
            std::size_t h3 = std::hash<int32_t>{}(k.domain_id);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct CacheEntry {
        AdminLevel level;
        std::chrono::steady_clock::time_point expiry;
    };

    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> m_cache;
    mutable std::shared_mutex m_mutex;
    std::chrono::seconds m_ttl{30};
};

// Standard Target Resource Resolvers
TargetResource resolveSessionOrg(const drogon::HttpRequestPtr& req);
TargetResource resolveDomainTarget(const drogon::HttpRequestPtr& req);
TargetResource resolveOrgTarget(const drogon::HttpRequestPtr& req);

// Scope-aware authorization filter
class ScopeFilter : public drogon::HttpFilter<ScopeFilter, false> {
public:
    explicit ScopeFilter(AdminLevel t_required_level = AdminLevel::ORG, TargetResourceResolver t_resolver = nullptr);

    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;

    AdminLevel requiredLevel() const noexcept {
        return m_required_level;
    }

private:
    AdminLevel m_required_level{AdminLevel::ORG};
    TargetResourceResolver m_resolver;
};

// Filter factory function
std::shared_ptr<ScopeFilter> requireLevel(AdminLevel level, TargetResourceResolver resolver = nullptr);

// Concrete filter classes for Drogon's string-based filter registration
class PlatformAdminFilter : public drogon::HttpFilter<PlatformAdminFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

class OrgAdminFilter : public drogon::HttpFilter<OrgAdminFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

class DomainAdminFilter : public drogon::HttpFilter<DomainAdminFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

// Backward-compatible AdminFilter: enforces at least ORG level (level >= 2 or platform)
class AdminFilter : public drogon::HttpFilter<AdminFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

} // namespace sgrn::datastore::filters
