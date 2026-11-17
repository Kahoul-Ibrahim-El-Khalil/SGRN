#pragma once
// admin.hpp — admin-endpoint wire types shared by server and clients.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::admin
{

struct AdminUserEntry {
    int64_t id{0};
    std::string email{};
    std::string first_name{};
    std::string family_name{};
    std::string domain{};
    std::string status{};
};

struct ServiceEntry {
    int64_t id{0};
    std::string name{};
    std::string token{};
    bool is_active{false};
    std::string domain{};
    std::string created_at{};
};

struct ServiceCredentials {
    std::string message{};
    std::string token{};
    std::string token_secret{};
};

/// A freshly created user, as returned by POST /api/v1/admin/users/register.
struct RegisteredUser {
    int32_t id{0};
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string role;
    std::string organisation;
    std::string status;
    std::string domain;
    std::string phone_number;
};

struct RegisterUserResponse {
    bool success{true};
    std::string message;
    RegisteredUser user;
};

struct NewUser {
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string password;
    std::string phone_number;
    std::string organisation;
    std::string status;
    std::string domain;
};

struct NewService {
    std::string name;
    std::string organisation;
    std::string kind;
    std::string domain;
};

// ─── Live sessions (GET /api/v1/admin/metaprobe/sessions) ─────────────────

struct ActiveSession {
    std::string token;
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::string ip;
    std::string created_at;
    std::string expires_at;
    std::string actor_type;
    std::string actor_name;
};

// ─── Webhooks (/api/v1/admin/webhooks) ─────────────────────────────────────

struct Webhook {
    int32_t id{0};
    std::string organisation;
    std::string url;
    bool is_active{false};
    std::string created_at;
};

struct RegisterWebhookRequest {
    std::string url;
    std::string secret;
    std::vector<std::string> events{"user.signin", "user.signout", "service.signin"};
};

struct WebhookRegistration {
    int32_t id{0};
    std::string url;
    std::string organisation;
    std::string created_at;
};

} // namespace sgrn::datastore::admin
