#pragma once

#include <cstdint>
#include <functional>
#include <json/json.h>
#include <optional>
#include <string>
#include <string_view>

namespace drogon
{
class HttpRequest;
using HttpRequestPtr = std::shared_ptr<HttpRequest>;
} // namespace drogon

namespace sgrn::datastore::filters
{
enum class UserRoleEnum : uint8_t { ADMIN = 0, USER = 1 };

enum class AdminLevel : uint8_t { NONE = 0, DOMAIN = 1, ORG = 2, PLATFORM = 3 };

inline auto format_as(UserRoleEnum t_role) {
    return static_cast<uint8_t>(t_role);
}

inline auto format_as(AdminLevel t_level) {
    return static_cast<uint8_t>(t_level);
}

inline std::string adminLevelToString(AdminLevel t_level) {
    switch (t_level) {
        case AdminLevel::PLATFORM:
            return "PLATFORM";
        case AdminLevel::ORG:
            return "ORG";
        case AdminLevel::DOMAIN:
            return "DOMAIN";
        case AdminLevel::NONE:
        default:
            return "NONE";
    }
}

inline AdminLevel adminLevelFromString(std::string_view t_sv) {
    if (t_sv == "PLATFORM" || t_sv == "platform") {
        return AdminLevel::PLATFORM;
    }
    if (t_sv == "ORG" || t_sv == "org" || t_sv == "organisation" || t_sv == "organization") {
        return AdminLevel::ORG;
    }
    if (t_sv == "DOMAIN" || t_sv == "domain") {
        return AdminLevel::DOMAIN;
    }
    return AdminLevel::NONE;
}

struct TargetResource {
    std::string organisation;
    std::optional<int32_t> domain_id{std::nullopt};
};

using TargetResourceResolver = std::function<TargetResource(const drogon::HttpRequestPtr&)>;

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