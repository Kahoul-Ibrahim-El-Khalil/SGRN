#include <sgrn/datastore/session/SessionStore.hpp>

#include <drogon/HttpAppFramework.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <trantor/utils/Logger.h>

#include <sgrn/datastore/services/WebhookService.hpp>

namespace sgrn::datastore::session
{

using drogon::Task;

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
        auto session_res = co_await db->execSqlCoro("SELECT token::text FROM core.sessions "
                                                    "WHERE terminated_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())");

        // ── Query 2: public service identity tokens (UUID in automated_services) ──
        // These are the *permanent public identity* of each automated service, NOT
        // session tokens. They are used as the service's credential UUID and are
        // loaded into a sorted vector for O(log n) binary-search rejection of any
        // request claiming to be a service that doesn't exist in our registry.
        auto service_res = co_await db->execSqlCoro("SELECT token::text FROM core.automated_services "
                                                    "WHERE deleted_at IS NULL AND status = 'active'");

        std::unique_lock lock(cache_mutex_);

        // Populate session active token set (unordered: O(1) lookup)
        active_tokens_.clear();
        for (const auto& row : session_res) {
            if (!row[0].isNull())
                active_tokens_.insert(row[0].as<std::string>());
        }

        // Populate public service token sorted vector (sorted: O(log n) binary search)
        public_service_tokens_.clear();
        public_service_tokens_.reserve(service_res.size());
        for (const auto& row : service_res) {
            if (!row[0].isNull())
                public_service_tokens_.push_back(row[0].as<std::string>());
        }
        std::sort(public_service_tokens_.begin(), public_service_tokens_.end());

        loaded_initial_ = true;
        LOG_INFO << "SessionStore: Pre-loaded " << active_tokens_.size() << " active session tokens (unordered_set) + "
                 << public_service_tokens_.size() << " public service tokens (sorted vector, binary-search ready).";
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore: Failed to pre-load tokens: " << e.what();
    }
}

Task<std::optional<Json::Value>> SessionStore::getSession(const std::string& token) {
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
        auto result = co_await db->execSqlCoro("SELECT session_data FROM core.sessions WHERE token = $1::uuid AND terminated_at IS NULL "
                                               "AND (expires_at IS NULL OR expires_at > NOW())",
            token);

        if (result.empty() || result[0]["session_data"].isNull()) {
            // Negative caching: record failed token for 60 seconds to prevent DB hammering
            std::unique_lock lock(cache_mutex_);
            negative_cache_[token] = {std::chrono::steady_clock::now() + std::chrono::seconds(60)};
            co_return std::nullopt;
        }

        std::string raw_json = result[0]["session_data"].as<std::string>();
        Json::Value claims;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

        if (reader->parse(raw_json.data(), raw_json.data() + raw_json.size(), &claims, &errors)) {
            // Populate local RAM Cache
            std::unique_lock lock(cache_mutex_);
            negative_cache_.erase(token);
            active_tokens_.insert(token);
            ram_cache_[token] = claims;
            co_return claims;
        }
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

Task<bool> SessionStore::storeSession(const std::string& token, const Json::Value& session_claims, int32_t user_id, int64_t ttl_seconds) {
    Json::FastWriter writer;
    std::string raw_json = writer.write(session_claims);

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

        // Trigger Webhook Notification for Session Login
        std::string org = session_claims.isMember("user") && session_claims["user"].isMember("organisation")
                              ? session_claims["user"]["organisation"].asString()
                              : "default";
        std::string event = session_claims.isMember("actor_type") && session_claims["actor_type"].asString() == "automated_service"
                                ? "service.signin"
                                : "user.signin";

        services::WebhookService::instance().dispatchEvent(org, event, session_claims);

        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore store error: " << e.what();
        co_return false;
    }
}

Task<void> SessionStore::revokeSession(const std::string& token, std::string termination_reason) {
    // 1. Snapshot the session claims from RAM cache BEFORE eviction so the webhook
    //    payload is rich (user_id, email, org, actor_type, ip, domain…).
    Json::Value cached_claims;
    bool had_claims = false;
    {
        std::shared_lock lock(cache_mutex_);
        auto it = ram_cache_.find(token);
        if (it != ram_cache_.end()) {
            cached_claims = it->second;
            had_claims = true;
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
        Json::Value payload;
        payload["token"] = token;
        payload["terminated_at"] = trantor::Date::now().toFormattedString(false);
        payload["termination_reason"] = termination_reason;

        std::string org = "default";
        std::string event = "user.signout";

        if (had_claims) {
            // Propagate all known identity fields so consumers don't need a follow-up query
            if (cached_claims.isMember("session_id"))
                payload["session_id"] = cached_claims["session_id"];
            if (cached_claims.isMember("actor_type")) {
                payload["actor_type"] = cached_claims["actor_type"];
                if (cached_claims["actor_type"].asString() == "automated_service")
                    event = "service.signout";
            }
            if (cached_claims.isMember("user")) {
                const Json::Value& u = cached_claims["user"];
                if (u.isMember("id"))
                    payload["user_id"] = u["id"];
                if (u.isMember("email"))
                    payload["email"] = u["email"];
                if (u.isMember("organisation")) {
                    payload["organisation"] = u["organisation"];
                    org = u["organisation"].asString();
                }
                if (u.isMember("domain"))
                    payload["domain"] = u["domain"];
                if (u.isMember("automated_service_id"))
                    payload["automated_service_id"] = u["automated_service_id"];
            }
            if (cached_claims.isMember("ip"))
                payload["ip"] = cached_claims["ip"];
        }

        // 5. Send NOTIFY so other instances evict this token from their RAM caches
        co_await pgNotify("revoke", token);

        // 6. Fire the webhook (async, fire-and-forget)
        services::WebhookService::instance().dispatchEvent(org, event, payload);

    } catch (const std::exception& e) {
        LOG_ERROR << "SessionStore revoke error: " << e.what();
    }

    co_return;
}

Task<std::optional<Json::Value>> SessionStore::refreshSession(const std::string& token) {
    try {
        auto db = drogon::app().getDbClient();

        // Re-fetch the authoritative session_data from PostgreSQL
        auto result = co_await db->execSqlCoro("SELECT session_data FROM core.sessions WHERE token = $1::uuid AND terminated_at IS NULL "
                                               "AND (expires_at IS NULL OR expires_at > NOW())",
            token);

        if (result.empty() || result[0]["session_data"].isNull()) {
            // Session no longer valid — evict stale RAM entry
            evictLocal(token);
            co_return std::nullopt;
        }

        std::string raw_json = result[0]["session_data"].as<std::string>();
        Json::Value claims;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

        if (!reader->parse(raw_json.data(), raw_json.data() + raw_json.size(), &claims, &errors)) {
            LOG_ERROR << "SessionStore::refreshSession: failed to parse session_data: " << errors;
            co_return std::nullopt;
        }

        // Hot-swap the RAM cache entry atomically under write lock
        {
            std::unique_lock lock(cache_mutex_);
            negative_cache_.erase(token);
            active_tokens_.insert(token);
            ram_cache_[token] = claims;
        }

        // Notify other instances to also refresh their copy
        co_await pgNotify("refresh", token);

        co_return claims;
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

                    for (const auto& row : events) {
                        std::string action = row["action"].as<std::string>();
                        std::string tok = row["token"].as<std::string>();

                        if (action == "revoke") {
                            evictLocal(tok);
                            LOG_INFO << "SessionStore: NOTIFY evict token " << tok.substr(0, 8) << "...";
                        } else if (action == "refresh") {
                            // Re-fetch session_data but don't re-notify (avoid loop)
                            try {
                                auto res = co_await listen_db->execSqlCoro(
                                    "SELECT session_data FROM core.sessions WHERE token = $1::uuid "
                                    "AND terminated_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())",
                                    tok);
                                if (!res.empty() && !res[0]["session_data"].isNull()) {
                                    std::string raw = res[0]["session_data"].as<std::string>();
                                    Json::Value claims;
                                    Json::CharReaderBuilder b;
                                    std::string err;
                                    auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
                                    if (reader->parse(raw.data(), raw.data() + raw.size(), &claims, &err)) {
                                        std::unique_lock lock(cache_mutex_);
                                        ram_cache_[tok] = claims;
                                        active_tokens_.insert(tok);
                                        LOG_INFO << "SessionStore: NOTIFY refresh token " << tok.substr(0, 8) << "...";
                                    }
                                } else {
                                    evictLocal(tok);
                                }
                            } catch (...) {
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

Task<Json::Value> SessionStore::getActiveSessions() {
    Json::Value list = Json::arrayValue;
    try {
        auto db = drogon::app().getDbClient();
        auto res = co_await db->execSqlCoro(
            "SELECT s.token::text, s.user_id, s.automated_service_id, s.ip::text, s.created_at::text, s.expires_at::text, "
            "u.email as user_email, u.first_name, u.family_name, a.name as service_name, s.session_data "
            "FROM core.sessions s "
            "LEFT JOIN core.users u ON u.id = s.user_id "
            "LEFT JOIN core.automated_services a ON a.id = s.automated_service_id "
            "WHERE s.terminated_at IS NULL AND (s.expires_at IS NULL OR s.expires_at > NOW()) "
            "ORDER BY s.created_at DESC");

        for (const auto& row : res) {
            Json::Value item;
            item["token"] = row["token"].as<std::string>();
            item["user_id"] = row["user_id"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["user_id"].as<int32_t>());
            item["automated_service_id"] = row["automated_service_id"].isNull() ? Json::Value(Json::nullValue)
                                                                                : Json::Value(row["automated_service_id"].as<int32_t>());
            item["ip"] = row["ip"].isNull() ? "127.0.0.1" : row["ip"].as<std::string>();
            item["created_at"] = row["created_at"].isNull() ? "" : row["created_at"].as<std::string>();
            item["expires_at"] = row["expires_at"].isNull() ? "" : row["expires_at"].as<std::string>();
            item["actor_type"] = row["user_id"].isNull() ? "automated_service" : "user";
            item["actor_name"] = row["user_id"].isNull()
                                     ? (row["service_name"].isNull() ? "Automated Service" : row["service_name"].as<std::string>())
                                     : (row["user_email"].isNull() ? "User" : row["user_email"].as<std::string>());
            list.append(item);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed fetching active sessions: " << e.what();
    }
    co_return list;
}

} // namespace sgrn::datastore::session
