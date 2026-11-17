#pragma once
// auth.hpp — authentication wire types shared by the datastore server and
// every C++ client. Plain aggregates: Glaze reflects them as-is for JSON,
// MessagePack and BEVE. Member order is part of the aggregate-initialisation
// contract used by the server (e.g. `UserSignInResponse{token, user}`), so do
// not reorder members without updating the call sites.

#include <sgrn/utils/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace sgrn::datastore::auth
{

// ─── Requests ───────────────────────────────────────────────────────────────

struct SignInRequest {
    std::string email;
    std::string password;
};

struct ServiceSignInRequest {
    std::string token;
    std::string secret;
};

struct UpdatePasswordRequest {
    std::string old_password;
    std::string new_password;
};

// ─── Responses ──────────────────────────────────────────────────────────────

struct UserRole {
    std::string name;
    int32_t code{0};
};

// ─── Session claims ─────────────────────────────────────────────────────────
// Keep this shape compatible with the user/service object historically stored
// in core.sessions.session_data. Handlers still consume that established shape.
struct SessionUserInfo {
    int32_t id{0};
    std::optional<int32_t> automated_service_id;
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string phone_number;
    std::string created_at;
    bool is_active{true};
    bool can_read_personal{true};
    bool can_write_personal{true};
    bool can_delete_personal{true};
    std::string status;
    std::string domain;
    std::string organisation;
    std::string name;
    std::string token;
    std::optional<sgrn::json::Value> metadata;
    UserRole role;
    bool is_platform_admin{false};
    int32_t admin_level{0};
    std::string admin_level_name{"NONE"};
    int64_t total_virtual_size{0};
    int64_t total_real_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
};

struct SessionClaims {
    SessionUserInfo user;
    std::string ip;
    std::string created_at;
    std::string expires_at;
    std::string actor_type{"user"};
    int64_t session_id{0};
};

struct UserDetails {
    int32_t id{0};
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string phone_number;
    std::string created_at;
    bool is_active{true};
    bool can_read_personal{true};
    bool can_write_personal{true};
    bool can_delete_personal{true};
    std::string status;
    std::string domain;
    std::string organisation;
    UserRole role;
    bool is_platform_admin{false};
    int32_t admin_level{0};
    std::string admin_level_name;
    int64_t total_virtual_size{0};
    int64_t total_real_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
};

/// GET /api/v1/query/user/info
struct UserInfo {
    int32_t id{0};
    std::string first_name;
    std::string family_name;
    std::string email;
    std::string organisation;
    std::string domain;
    std::string status;
};

struct UserSignInResponse {
    std::string token;
    UserDetails user;
};

struct AutomatedServiceDetails {
    int32_t automated_service_id{0};
    std::string organisation;
    std::string name;
    std::string token;
    std::string status;
    std::string domain;
    std::optional<sgrn::json::Value> metadata;
    std::string created_at;
    std::string updated_at;
    int64_t total_virtual_size{0};
    int64_t total_real_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
    UserRole role;
    bool is_platform_admin{false};
    int32_t admin_level{0};
    std::string admin_level_name;
};

struct ServiceSignInResponse {
    std::string token;
    int64_t session_id{0};
    AutomatedServiceDetails automated_service;
};

} // namespace sgrn::datastore::auth
