#pragma once

#include <cstdint>
#include <json/json.h>
#include <string>

namespace sgrn::datastore::filters
{
enum class UserRoleEnum : uint8_t { ADMIN = 0, USER = 1 };

inline auto format_as(UserRoleEnum t_role) {
    return static_cast<uint8_t>(t_role);
}

struct UserRole {
    UserRoleEnum value_;

    UserRole(UserRoleEnum t_value)
        : value_(t_value) {
    }

    std::string toString() const {
        switch (value_) {
            case UserRoleEnum::ADMIN:
                return "admin";
            case UserRoleEnum::USER:
                return "user";
            default:
                return "unknown";
        }
    }

    Json::Value jsonify() const {
        Json::Value result;
        result["role"] = toString();
        result["roleCode"] = static_cast<uint8_t>(value_);
        return result;
    }
};

// 2. Data Structures for Payloads
struct UserAuthPayload {
    std::string email;
    std::string password;
};

struct SgrnSession {
    UserRole status;
    int32_t id;
    std::string token;
    std::string email;

    uint32_t organisation_id;
};
} // namespace sgrn::datastore::filters