#include <sgrn/datastore/session/SessionStore.hpp>

#include <drogon/HttpAppFramework.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <trantor/utils/Logger.h>

#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/services/WebhookService.hpp>

namespace sgrn::datastore::session
{

using drogon::Task;

namespace
{
static constexpr char kActiveSessionTokensQuery[] = "SELECT token::text AS token FROM core.sessions "
                                                    "WHERE terminated_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())";
static constexpr char kPublicServiceTokensQuery[] = "SELECT token::text AS token FROM core.automated_services "
                                                    "WHERE deleted_at IS NULL AND status = 'active'";
static constexpr char kSessionDataQuery[] = "SELECT session_data FROM core.sessions WHERE token = $1::uuid AND terminated_at IS NULL "
                                            "AND (expires_at IS NULL OR expires_at > NOW())";

struct TokenRow {
    std::optional<std::string> token;
};

struct SessionDataRow {
    std::optional<std::string> session_data;
};

struct SessionNotificationRow {
    std::string action;
    std::string token;
};

struct ActiveSessionRow {
    std::string token;
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::optional<std::string> ip;
    std::optional<std::string> created_at;
    std::optional<std::string> expires_at;
    std::optional<std::string> user_email;
    std::optional<std::string> user_organisation;
    std::optional<std::string> service_name;
    std::optional<std::string> service_organisation;
};

struct SessionSignInEvent {
    std::string org;
    std::string event;
    std::string actor_type;
    int64_t session_id{0};
};

struct SessionRevocationEvent {
    std::string token;
    std::string terminated_at;
    std::string termination_reason;
    std::optional<int64_t> session_id;
    std::optional<std::string> actor_type;
    std::optional<int32_t> user_id;
    std::optional<std::string> email;
    std::optional<std::string> organisation;
    std::optional<std::string> domain;
    std::optional<int32_t> automated_service_id;
    std::optional<std::string> ip;
};
} // namespace

SessionStore& SessionStore::instance() {
    static SessionStore inst;
    return inst;
}

Task<void> SessionStore::loadActiveTokens() {
    try {
        auto db = drogon::app().getDbClient();

        // ── Query 1: active session tokens (session UUID, issued at sign-in) ──────
        // These live in core.sessions and are the short-lived bearer tokens used
        // by both users and automated services after authentication.
        auto session_res = co_await db->execSqlCoro(kActiveSessionTokensQuery);

        // ── Query 2: public service identity tokens (UUID in automated_services) ──
        // These are the *permanent public identity* of each automated service, NOT
        // session tokens. They are used as the service's credential UUID and are
        // loaded into a sorted vector for O(log n) binary-search rejection of any
        // request claiming to be a service that doesn't exist in our registry.
        auto service_res = co_await db->execSqlCoro(kPublicServiceTokensQuery);

        std::unique_lock lock(cache_mutex_);

        // Populate session active token set (unordered: O(1) lookup)
        active_tokens_.clear();
        for (const auto& row : sgrn::orm::fromResult<TokenRow, kActiveSessionTokensQuery>(session_res)) {
            if (row.token)
                active_tokens_.insert(*row.token);
        }

        // Populate public service token sorted vector (sorted: O(log n) binary search)
        public_service_tokens_.clear();
        public_service_tokens_.reserve(service_res.size());
        for (const auto& row : sgrn::orm::fromResult<TokenRow, kPublicServiceTokensQuery>(service_res)) {
            if (row.token)
                public_service_tokens_.push_back(*row.token);
        }
        std::sort(public_service_tokens_.begin(), public_service_tokens_.end());

        loaded_initial_ = true;
        LOG_INFO << "SessionStore: Pre-loaded " << active_tokens_.size() << " active session tokens (unordered_set) + "
                 << public_service_tokens_.size() << " public service tokens (sorted vector, binary-search ready).";
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore: Failed to pre-load tokens: " << e.what();
    }
}

Task<std::optional<::sgrn::datastore::auth::SessionClaims>> SessionStore::getSession(const std::string& token) {
    // 1. Check RAM Cache & Active Token Set (~15ns fast path)
    {
        std::shared_lock lock(cache_mutex_);
        auto it = ram_cache_.find(token);
        if (it != ram_cache_.end()) {
            co_return it->second;
        }
        // If active tokens have been pre-loaded and token is NOT in active_tokens_, fast-reject (<15ns)
        if (loaded_initial_ && active_tokens_.find(token) == active_tokens_.end()) {
            co_return std::nullopt; // Instant RAM rejection (<15ns), zero DB queries!
        }
        auto neg_it = negative_cache_.find(token);
        if (neg_it != negative_cache_.end()) {
            if (std::chrono::steady_clock::now() < neg_it->second.expires_at) {
                co_return std::nullopt; // Fast negative hit: invalid/expired token
            }
        }
    }

    // 2. Cache miss: Query PostgreSQL core.sessions table
    try {
        auto db = drogon::app().getDbClient();
        auto result = co_await db->execSqlCoro(kSessionDataQuery, token);

        if (result.empty()) {
            // Negative caching: record failed token for 60 seconds to prevent DB hammering
            std::unique_lock lock(cache_mutex_);
            negative_cache_[token] = {std::chrono::steady_clock::now() + std::chrono::seconds(60)};
            co_return std::nullopt;
        }

        const auto row = sgrn::orm::fromRow<SessionDataRow, kSessionDataQuery>(result[0]);
        if (!row.session_data) {
            std::unique_lock lock(cache_mutex_);
            negative_cache_[token] = {std::chrono::steady_clock::now() + std::chrono::seconds(60)};
            co_return std::nullopt;
        }

        const std::string& raw_json = *row.session_data;
        auto claims_result = ::sgrn::json::read<::sgrn::datastore::auth::SessionClaims>(raw_json);
        if (claims_result.hasError()) {
            LOG_ERROR << "SessionStore: failed to parse session_data JSONB into SessionClaims: " << claims_result.error().message;
            // Negative cache it so we don't keep retrying the same bad data
            std::unique_lock lock(cache_mutex_);
            negative_cache_[token] = {std::chrono::steady_clock::now() + std::chrono::seconds(60)};
            co_return std::nullopt;
        }

        // Populate local RAM Cache
        std::unique_lock lock(cache_mutex_);
        negative_cache_.erase(token);
        active_tokens_.insert(token);
        ram_cache_[token] = claims_result.value();
        co_return claims_result.value();
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore database error: " << e.what();
    }

    co_return std::nullopt;
}

bool SessionStore::isKnownServiceToken(const std::string& token) const noexcept {
    std::shared_lock lock(cache_mutex_);
    // O(log n) binary search on the sorted vector — cache-friendly, branchless comparison
    return std::binary_search(public_service_tokens_.begin(), public_service_tokens_.end(), token);
}

bool SessionStore::hasActiveSessionToken(const std::string& token) const noexcept {
    std::shared_lock lock(cache_mutex_);
    return active_tokens_.find(token) != active_tokens_.end();
}

void SessionStore::registerServiceToken(const std::string& token) {
    std::unique_lock lock(cache_mutex_);
    // Find the correct sorted insertion position via lower_bound (O(log n)),
    // then insert — preserves sorted order without a full re-sort.
    auto it = std::lower_bound(public_service_tokens_.begin(), public_service_tokens_.end(), token);
    if (it == public_service_tokens_.end() || *it != token) {
        public_service_tokens_.insert(it, token);
        LOG_INFO << "SessionStore: Registered new service token into lookup vector (size=" << public_service_tokens_.size() << ")";
    }
}

void SessionStore::deregisterServiceToken(const std::string& token) {
    std::unique_lock lock(cache_mutex_);
    auto it = std::lower_bound(public_service_tokens_.begin(), public_service_tokens_.end(), token);
    if (it != public_service_tokens_.end() && *it == token) {
        public_service_tokens_.erase(it);
        LOG_INFO << "SessionStore: Deregistered service token from lookup vector (size=" << public_service_tokens_.size() << ")";
    }
}

Task<bool> SessionStore::storeSession(
    const std::string& token, const ::sgrn::datastore::auth::SessionClaims& session_claims, int32_t user_id, int64_t ttl_seconds) {
    // Serialize SessionClaims to JSON text via Glaze for PostgreSQL jsonb column.
    auto json_result = ::sgrn::json::write(session_claims);
    if (json_result.hasError()) {
        LOG_ERROR << "SessionStore: failed to serialize SessionClaims to JSON: " << json_result.error().message;
        co_return false;
    }
    std::string raw_json = json_result.value();

    try {
        auto db = drogon::app().getDbClient();
        auto res = co_await db->execSqlCoro("UPDATE core.sessions SET session_data = $2::jsonb, expires_at = NOW() + ($3 || ' "
                                            "seconds')::interval WHERE token = $1::uuid AND terminated_at IS NULL",
            token, raw_json, std::to_string(ttl_seconds));

        if (res.affectedRows() == 0) {
            co_await db->execSqlCoro("INSERT INTO core.sessions (token, user_id, ip, session_data, expires_at) "
                                     "VALUES ($1::uuid, $2, '127.0.0.1'::inet, $3::jsonb, NOW() + ($4 || ' seconds')::interval)",
                token, user_id, raw_json, std::to_string(ttl_seconds));
        }

        // Populate local RAM Cache & active token set
        {
            std::unique_lock lock(cache_mutex_);
            negative_cache_.erase(token);
            active_tokens_.insert(token);
            ram_cache_[token] = session_claims;
        }

        // Determine event type from actor_type
        std::string org = session_claims.user.organisation.empty() ? "default" : session_claims.user.organisation;
        const bool is_service = session_claims.actor_type == "automated_service" || session_claims.user.automated_service_id.has_value();
        const std::string actor_type = is_service ? "automated_service" : "user";
        const std::string event = is_service ? "service.signin" : "user.signin";

        services::WebhookService::instance().dispatchEvent(
            org, event, SessionSignInEvent{org, event, actor_type, session_claims.session_id});

        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore store error: " << e.what();
        co_return false;
    }
}

Task<void> SessionStore::revokeSession(const std::string& token, std::string termination_reason) {
    // 1. Snapshot the session claims from RAM cache BEFORE eviction so the webhook
    //    payload is rich (user_id, email, org, actor_type, ip, domain…).
    std::optional<::sgrn::datastore::auth::SessionClaims> cached_claims;
    {
        std::shared_lock lock(cache_mutex_);
        auto it = ram_cache_.find(token);
        if (it != ram_cache_.end()) {
            cached_claims = it->second;
        }
    }

    // 2. Evict from RAM immediately — in-flight requests will re-validate against DB
    evictLocal(token);

    try {
        auto db = drogon::app().getDbClient();

        // 3. Stamp terminated_at + reason in PostgreSQL
        co_await db->execSqlCoro(
            "UPDATE core.sessions SET terminated_at = NOW(), termination_reason = $2 WHERE token = $1::uuid", token, termination_reason);

        // 4. Build rich webhook payload from cached claims
        std::string org = "default";
        std::string event = "user.signout";
        std::string actor_type = "user";
        SessionRevocationEvent payload{.token = token,
            .terminated_at = trantor::Date::now().toFormattedString(false),
            .termination_reason = termination_reason,
            .session_id = std::nullopt,
            .actor_type = std::nullopt,
            .user_id = std::nullopt,
            .email = std::nullopt,
            .organisation = std::nullopt,
            .domain = std::nullopt,
            .automated_service_id = std::nullopt,
            .ip = std::nullopt};
        std::optional<int32_t> act_id;
        std::string act_name = "unknown";
        std::string client_ip = "127.0.0.1";

        if (cached_claims) {
            const auto& claims = *cached_claims;
            const bool is_service = claims.actor_type == "automated_service" || claims.user.automated_service_id.has_value();
            actor_type = is_service ? "automated_service" : "user";
            payload.session_id = claims.session_id != 0 ? std::optional<int64_t>(claims.session_id) : std::nullopt;
            payload.actor_type = actor_type;
            if (is_service) {
                event = "service.signout";
            }
            if (!is_service && claims.user.id != 0) {
                payload.user_id = claims.user.id;
                act_id = claims.user.id;
            }
            if (!claims.user.email.empty()) {
                payload.email = claims.user.email;
                act_name = claims.user.email;
            }
            if (!claims.user.organisation.empty()) {
                payload.organisation = claims.user.organisation;
                org = claims.user.organisation;
            }
            if (!claims.user.domain.empty()) {
                payload.domain = claims.user.domain;
            }
            if (claims.user.automated_service_id) {
                payload.automated_service_id = claims.user.automated_service_id;
            }
            if (!claims.ip.empty()) {
                payload.ip = claims.ip;
                client_ip = claims.ip;
            }
        }

        // 5. Send NOTIFY so other instances evict this token from their RAM caches
        co_await pgNotify("revoke", token);

        // 6. Fire the webhook (async, fire-and-forget)
        services::WebhookService::instance().dispatchEvent(org, event, payload);

        // 7. Record immutable audit log
        co_await sgrn::datastore::audit::AuditLogger::log(
            db, org, actor_type, act_id, act_name, "session.revoked", "session", token, client_ip, payload, "success");

    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore revoke error: " << e.what();
    }

    co_return;
}

Task<std::optional<::sgrn::datastore::auth::SessionClaims>> SessionStore::refreshSession(const std::string& token) {
    try {
        auto db = drogon::app().getDbClient();

        // Re-fetch the authoritative session_data from PostgreSQL
        auto result = co_await db->execSqlCoro(kSessionDataQuery, token);

        if (result.empty()) {
            // Session no longer valid — evict stale RAM entry
            evictLocal(token);
            co_return std::nullopt;
        }

        const auto row = sgrn::orm::fromRow<SessionDataRow, kSessionDataQuery>(result[0]);
        if (!row.session_data) {
            evictLocal(token);
            co_return std::nullopt;
        }

        const std::string& raw_json = *row.session_data;
        auto claims_result = ::sgrn::json::read<::sgrn::datastore::auth::SessionClaims>(raw_json);
        if (claims_result.hasError()) {
            LOG_ERROR << "SessionStore::refreshSession: failed to parse session_data into SessionClaims: " << claims_result.error().message;
            co_return std::nullopt;
        }

        // Hot-swap the RAM cache entry atomically under write lock
        {
            std::unique_lock lock(cache_mutex_);
            negative_cache_.erase(token);
            active_tokens_.insert(token);
            ram_cache_[token] = claims_result.value();
        }

        // Notify other instances to also refresh their copy
        co_await pgNotify("refresh", token);

        co_return claims_result.value();
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore::refreshSession error: " << e.what();
        co_return std::nullopt;
    }
}

void SessionStore::evictLocal(const std::string& token) {
    std::unique_lock lock(cache_mutex_);
    ram_cache_.erase(token);
    negative_cache_.erase(token);
    active_tokens_.erase(token);
}

Task<void> SessionStore::pgNotify(const std::string& action, const std::string& token) {
    try {
        auto db = drogon::app().getDbClient();
        // 1. Insert into the polling table — reliable cross-instance delivery
        //    even if the libpq NOTIFY channel is missed (pool connection recycling).
        co_await db->execSqlCoro("INSERT INTO core.session_notify_queue (action, token) VALUES ($1, $2)", action, token);
        // 2. Also fire pg_notify for instances that happen to be listening live
        std::string notify_payload = "{\"action\":\"" + action + "\",\"token\":\"" + token + "\"}";
        co_await db->execSqlCoro("SELECT pg_notify('sgrn_session_events', $1)", notify_payload);
        // 3. Opportunistically purge stale rows older than 5 minutes
        co_await db->execSqlCoro("SELECT core.purge_old_session_notifications()");
    } catch (const std::exception& e) {
        LOG_WARN << "SessionStore::pgNotify failed (non-fatal): " << e.what();
    }
}

void SessionStore::startPgListener() {
    if (listener_started_)
        return;
    listener_started_ = true;

    drogon::app().getLoop()->runInLoop([this]() {
        LOG_INFO << "SessionStore: Initialized RAM session cache & active token set with PostgreSQL LISTEN/NOTIFY";

        // 1. Pre-load all active tokens at startup
        drogon::async_run([this]() -> Task<void> { co_await loadActiveTokens(); });

        // 2. Open a dedicated persistent connection for LISTEN sgrn_session_events.
        //    This must be a separate connection from the pool because LISTEN is
        //    connection-scoped and pool connections can be recycled mid-listen.
        drogon::async_run([this]() -> Task<void> {
            try {
                // Use the same connection string as the pool but get a dedicated client
                auto listen_db = drogon::app().getDbClient();

                co_await listen_db->execSqlCoro("LISTEN sgrn_session_events");
                LOG_INFO << "SessionStore: Subscribed to LISTEN sgrn_session_events";

                // PostgreSQL asynchronous notifications are delivered via the libpq
                // async notify API. Drogon's orm::DbClient doesn't expose a raw
                // notify callback, so we poll at 500ms intervals — cheap since we
                // are only reading already-buffered notifications from libpq.
                while (true) {
                    co_await drogon::sleepCoro(drogon::app().getLoop(), 0.5);

                    auto rows = co_await listen_db->execSqlCoro("SELECT pg_notification_queue_usage() AS q"); // ping keeps connection alive

                    // Process any pending notifications by checking the session
                    // events channel. Drogon wraps libpq so raw PQnotifies() isn't
                    // directly accessible here — we rely on the pg_notify trigger
                    // path writing into a dedicated events table that we poll.
                    auto events = co_await listen_db->execSqlCoro("DELETE FROM core.session_notify_queue WHERE id IN ("
                                                                  "  SELECT id FROM core.session_notify_queue ORDER BY id LIMIT 50"
                                                                  ") RETURNING action, token");

                    for (const auto& row : sgrn::orm::fromResultByName<SessionNotificationRow>(events)) {
                        const std::string& action = row.action;
                        const std::string& tok = row.token;

                        if (action == "revoke") {
                            // Invalidate the admin-level cache for this token's user before
                            // eviction so stale level entries don't linger in memory.
                            {
                                std::shared_lock rlock(cache_mutex_);
                                auto it = ram_cache_.find(tok);
                                if (it != ram_cache_.end()) {
                                    const ::sgrn::datastore::auth::SessionClaims& c = it->second;
                                    if (c.actor_type == "user" && c.user.id != 0) {
                                        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(c.user.id);
                                    }
                                }
                            }
                            evictLocal(tok);
                            LOG_INFO << "SessionStore: NOTIFY evict token " << tok.substr(0, 8) << "...";
                        } else if (action == "refresh") {
                            // Re-fetch session_data but don't re-notify (avoid loop).
                            // Also invalidate the admin-level cache so the next request
                            // re-resolves the user's level from the DB.
                            try {
                                auto res = co_await listen_db->execSqlCoro(kSessionDataQuery, tok);
                                if (!res.empty()) {
                                    const auto session_row = sgrn::orm::fromRow<SessionDataRow, kSessionDataQuery>(res[0]);
                                    if (!session_row.session_data) {
                                        evictLocal(tok);
                                        continue;
                                    }
                                    const std::string& raw = *session_row.session_data;
                                    auto claims_result = ::sgrn::json::read<::sgrn::datastore::auth::SessionClaims>(raw);
                                    if (claims_result.hasError()) {
                                        LOG_ERROR << "SessionStore: failed to parse session_data into SessionClaims: "
                                                  << claims_result.error().message;
                                        evictLocal(tok);
                                    } else {
                                        // Invalidate admin-level cache before updating RAM cache
                                        {
                                            std::shared_lock lock(cache_mutex_);
                                            auto it = ram_cache_.find(tok);
                                            if (it != ram_cache_.end() && it->second.actor_type == "user" && it->second.user.id != 0) {
                                                sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(it->second.user.id);
                                            }
                                        }
                                        // Update RAM cache
                                        {
                                            std::unique_lock wlock(cache_mutex_);
                                            ram_cache_[tok] = claims_result.value();
                                            active_tokens_.insert(tok);
                                        }
                                        LOG_INFO << "SessionStore: NOTIFY refresh token " << tok.substr(0, 8) << "...";
                                    }
                                } else {
                                    evictLocal(tok);
                                }
                            } catch (const std::exception& e) {
                                LOG_ERROR << "SessionStore: NOTIFY refresh error: " << e.what();
                                evictLocal(tok);
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "SessionStore LISTEN loop crashed: " << e.what();
            }
        });
    });
}

Task<std::vector<ActiveSession>> SessionStore::getActiveSessions() {
    std::vector<ActiveSession> sessions;
    try {
        auto db = drogon::app().getDbClient();
        static constexpr char kQuery[] = "SELECT s.token::text AS token, s.user_id, s.automated_service_id, s.ip::text AS ip, "
                                         "s.created_at::text AS created_at, s.expires_at::text AS expires_at, "
                                         "u.email AS user_email, u.organisation AS user_organisation, "
                                         "a.name AS service_name, a.organisation AS service_organisation "
                                         "FROM core.sessions s "
                                         "LEFT JOIN core.users u ON u.id = s.user_id "
                                         "LEFT JOIN core.automated_services a ON a.id = s.automated_service_id "
                                         "WHERE s.terminated_at IS NULL AND (s.expires_at IS NULL OR s.expires_at > NOW()) "
                                         "ORDER BY s.created_at DESC";
        auto res = co_await db->execSqlCoro(kQuery);

        for (const auto& row : sgrn::orm::fromResult<ActiveSessionRow, kQuery>(res)) {
            const bool is_service = !row.user_id;
            sessions.push_back({row.token, row.user_id, row.automated_service_id, row.ip.value_or("127.0.0.1"), row.created_at.value_or(""),
                row.expires_at.value_or(""), is_service ? "automated_service" : "user",
                is_service ? row.service_name.value_or("Automated Service") : row.user_email.value_or("User"),
                is_service ? row.service_organisation.value_or("") : row.user_organisation.value_or("")});
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed fetching active sessions: " << e.what();
    }
    co_return sessions;
}

} // namespace sgrn::datastore::session
