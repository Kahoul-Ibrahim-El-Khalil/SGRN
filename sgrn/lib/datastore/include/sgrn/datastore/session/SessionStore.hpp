#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sgrn::datastore::session
{

class SessionStore {
public:
    static SessionStore& instance();

    // High-performance session lookup: checks RAM cache first (~20ns), falls back to PostgreSQL
    drogon::Task<std::optional<Json::Value>> getSession(const std::string& token);

    // Persists session in PostgreSQL core.sessions table & populates RAM cache
    drogon::Task<bool> storeSession(
        const std::string& token, const Json::Value& session_claims, int32_t user_id, int64_t ttl_seconds = 86400);

    // Revokes session in PostgreSQL, fires user.signout webhook with full claims, sends NOTIFY
    drogon::Task<void> revokeSession(const std::string& token, std::string termination_reason = "logout");

    // Re-fetches session_data from PostgreSQL and hot-swaps the RAM cache entry.
    // Call after any handler that mutates user quota/permission columns so the next
    // request sees fresh values without requiring a re-login.
    drogon::Task<std::optional<Json::Value>> refreshSession(const std::string& token);

    // Evicts token from local RAM cache
    void evictLocal(const std::string& token);

    // Pre-loads all active session tokens into RAM set + all public automated
    // service tokens into a sorted vector (for O(log n) binary-search rejection)
    drogon::Task<void> loadActiveTokens();

    // O(log n) binary search: returns true if token is a known public service token
    // Safe to call from any thread (acquires shared lock internally)
    bool isKnownServiceToken(const std::string& token) const noexcept;

    // Register a newly created / rotated service public token into the sorted vector.
    // Uses lower_bound insertion to preserve sorted order: O(log n) find + O(n) shift.
    // Call after INSERT into core.automated_services.
    void registerServiceToken(const std::string& token);

    // Remove a service public token (on deletion or rotation of old token).
    // Uses lower_bound to locate it in O(log n) then erases: O(n) shift.
    // Call before/after UPDATE/DELETE on core.automated_services.
    void deregisterServiceToken(const std::string& token);

    // Start background PostgreSQL LISTEN/NOTIFY loop for cross-instance cache
    // invalidation (revoke) and refresh (session_data updated by admin writes).
    void startPgListener();

    // Retrieves full details of active live sessions from database / RAM cache
    drogon::Task<Json::Value> getActiveSessions();

private:
    SessionStore() = default;

    struct NegativeEntry {
        std::chrono::steady_clock::time_point expires_at;
    };

    // Session RAM cache: token → claims JSON
    std::unordered_map<std::string, Json::Value> ram_cache_;

    // Negative cache: tokens confirmed invalid, suppressed for 60s to block DB hammering
    std::unordered_map<std::string, NegativeEntry> negative_cache_;

    // Active session tokens set: O(1) amortised lookup — populated at boot + on store/revoke
    std::unordered_set<std::string> active_tokens_;

    // Sorted vector of public automated service tokens (the UUID identity, not the session token).
    // Kept sorted at all times → O(log n) std::lower_bound binary search, excellent cache locality.
    // These never change at runtime (only service provisioning adds/removes them), so they are
    // refreshed at boot and on NOTIFY service_token_update.
    std::vector<std::string> public_service_tokens_;

    mutable std::shared_mutex cache_mutex_;
    bool listener_started_{false};
    bool loaded_initial_{false};

    // Internal: send NOTIFY sgrn_session_events via an ad-hoc DB query.
    // action: "revoke" | "refresh"
    drogon::Task<void> pgNotify(const std::string& action, const std::string& token);
};

} // namespace sgrn::datastore::session
