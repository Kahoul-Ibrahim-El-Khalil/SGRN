#pragma once

#include <fmt/format.h>
#include <sgrn/utils/json.hpp>
#include <rapidjson/document.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace sgrn::sdk
{

using TelemetryValue = std::variant<int64_t, double, std::string, bool>;
using TelemetryPayload = std::map<std::string, TelemetryValue>;

enum class StorageScope {
    Auto,
    Personal,
    Users,
    AutomatedServices,
};

enum class DriveItemType {
    File,
    Folder,
};

struct DrivePathNode {
    std::optional<int64_t> id_{};
    std::string name_{};
    std::string path{};
    std::string display_name{};
};

struct DriveFolderInfo {
    std::optional<int64_t> id_{};
    std::string name_{};
    std::string path{};
    std::string display_name{};
    std::string token{};
    std::string kind{};
    std::string created_at{};
};

struct DriveFileInfo {
    int64_t id_{};
    std::string name_{};
    std::string path{};
    std::string extension{};
    int64_t size{};
    int64_t original_size{};
    std::string created_at{};
};

struct DriveListing {
    std::string path{};
    std::vector<DrivePathNode> trail{};
    std::vector<DriveFolderInfo> folders{};
    std::vector<DriveFileInfo> files{};
};

struct ObjectInfo {
    int32_t id_{};
    std::string name_{};
    std::string status_{};
    rapidjson::Document metadata_{};
};

namespace detail
{

inline std::string storageScopeToString(StorageScope t_scope) {
    switch (t_scope) {
        case StorageScope::Personal:
            return "personal";
        case StorageScope::Users:
            return "users";
        case StorageScope::AutomatedServices:
            return "automated_services";
        case StorageScope::Auto:
        default:
            return "auto";
    }
}

inline std::optional<StorageScope> storageScopeFromString(const std::string& t_scope) {
    if (t_scope == "personal") {
        return StorageScope::Personal;
    }
    if (t_scope == "users") {
        return StorageScope::Users;
    }
    if (t_scope == "automated_services" || t_scope == "automated-services") {
        return StorageScope::AutomatedServices;
    }
    if (t_scope == "auto" || t_scope.empty()) {
        return StorageScope::Auto;
    }
    return std::nullopt;
}

inline std::string driveItemTypeToString(DriveItemType t_type) {
    switch (t_type) {
        case DriveItemType::Folder:
            return "folder";
        case DriveItemType::File:
        default:
            return "file";
    }
}

inline std::string urlEncodeComponent(const std::string& t_input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(t_input.size() * 3);
    for (unsigned char c : t_input) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                          c == '~' || c == '/' || c == ':';
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

inline std::string buildQueryString(const std::vector<std::pair<std::string, std::string>>& t_params) {
    std::string out;
    bool first = true;
    for (const auto& [key, t_value] : t_params) {
        if (t_value.empty()) {
            continue;
        }
        out += first ? '?' : '&';
        first = false;
        out += key;
        out += '=';
        out += urlEncodeComponent(t_value);
    }
    return out;
}

inline DrivePathNode parseDrivePathNode(const rapidjson::Value& t_value) {
    DrivePathNode node;
    if (t_value.HasMember("id") && !t_value["id"].IsNull()) {
        node.id_ = t_value["id"].IsInt64() ? t_value["id"].GetInt64() : static_cast<int64_t>(t_value["id"].GetInt());
    }
    if (t_value.HasMember("name") && t_value["name"].IsString()) {
        node.name_ = t_value["name"].GetString();
    }
    if (t_value.HasMember("path") && t_value["path"].IsString()) {
        node.path = t_value["path"].GetString();
    }
    if (t_value.HasMember("display_name") && t_value["display_name"].IsString()) {
        node.display_name = t_value["display_name"].GetString();
    }
    return node;
}

inline DriveFolderInfo parseDriveFolderInfo(const rapidjson::Value& t_value) {
    DriveFolderInfo folder;
    if (t_value.HasMember("id") && !t_value["id"].IsNull()) {
        folder.id_ = t_value["id"].IsInt64() ? t_value["id"].GetInt64() : static_cast<int64_t>(t_value["id"].GetInt());
    }
    if (t_value.HasMember("name") && t_value["name"].IsString()) {
        folder.name_ = t_value["name"].GetString();
    }
    if (t_value.HasMember("path") && t_value["path"].IsString()) {
        folder.path = t_value["path"].GetString();
    }
    if (t_value.HasMember("display_name") && t_value["display_name"].IsString()) {
        folder.display_name = t_value["display_name"].GetString();
    }
    if (t_value.HasMember("token") && t_value["token"].IsString()) {
        folder.token = t_value["token"].GetString();
    }
    if (t_value.HasMember("kind") && t_value["kind"].IsString()) {
        folder.kind = t_value["kind"].GetString();
    }
    if (t_value.HasMember("created_at") && t_value["created_at"].IsString()) {
        folder.created_at = t_value["created_at"].GetString();
    }
    return folder;
}

inline DriveFileInfo parseDriveFileInfo(const rapidjson::Value& t_value) {
    DriveFileInfo file;
    if (t_value.HasMember("id") && !t_value["id"].IsNull()) {
        file.id_ = t_value["id"].IsInt64() ? t_value["id"].GetInt64() : static_cast<int64_t>(t_value["id"].GetInt());
    }
    if (t_value.HasMember("name") && t_value["name"].IsString()) {
        file.name_ = t_value["name"].GetString();
    }
    if (t_value.HasMember("path") && t_value["path"].IsString()) {
        file.path = t_value["path"].GetString();
    }
    if (t_value.HasMember("extension") && t_value["extension"].IsString()) {
        file.extension = t_value["extension"].GetString();
    }
    if (t_value.HasMember("size") && !t_value["size"].IsNull()) {
        file.size = t_value["size"].IsInt64() ? t_value["size"].GetInt64() : static_cast<int64_t>(t_value["size"].GetInt());
    }
    if (t_value.HasMember("original_size") && !t_value["original_size"].IsNull()) {
        file.original_size = t_value["original_size"].IsInt64() ? t_value["original_size"].GetInt64()
                                                                : static_cast<int64_t>(t_value["original_size"].GetInt());
    }
    if (t_value.HasMember("created_at") && t_value["created_at"].IsString()) {
        file.created_at = t_value["created_at"].GetString();
    }
    return file;
}

inline DriveListing parseDriveListing(const rapidjson::Value& t_value) {
    DriveListing listing;
    if (t_value.HasMember("path") && t_value["path"].IsString()) {
        listing.path = t_value["path"].GetString();
    }
    if (t_value.HasMember("trail") && t_value["trail"].IsArray()) {
        for (const auto& entry : t_value["trail"].GetArray()) {
            listing.trail.push_back(parseDrivePathNode(entry));
        }
    }
    if (t_value.HasMember("folders") && t_value["folders"].IsArray()) {
        for (const auto& entry : t_value["folders"].GetArray()) {
            listing.folders.push_back(parseDriveFolderInfo(entry));
        }
    }
    if (t_value.HasMember("files") && t_value["files"].IsArray()) {
        for (const auto& entry : t_value["files"].GetArray()) {
            listing.files.push_back(parseDriveFileInfo(entry));
        }
    }
    return listing;
}

inline std::string jsonCompact(const rapidjson::Value& t_value) {
    return sgrn::utils::json::serializeCompact(t_value);
}

inline std::string telemetryValueToString(const TelemetryValue& t_value) {
    return std::visit(
        [](const auto& t_item) -> std::string {
            using T = std::decay_t<decltype(t_item)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return fmt::format("\"{}\"", t_item);
            } else if constexpr (std::is_same_v<T, bool>) {
                return t_item ? "true" : "false";
            } else {
                return fmt::format("{}", t_item);
            }
        },
        t_value);
}

} // namespace detail

} // namespace sgrn::sdk

template <>
struct fmt::formatter<sgrn::sdk::TelemetryValue> : formatter<std::string_view> {
    auto format(const sgrn::sdk::TelemetryValue& t_value, format_context& t_ctx) const {
        return formatter<std::string_view>::format(sgrn::sdk::detail::telemetryValueToString(t_value), t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::sdk::TelemetryPayload> : formatter<std::string_view> {
    auto format(const sgrn::sdk::TelemetryPayload& t_payload, format_context& t_ctx) const {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, t_value] : t_payload) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += fmt::format("\"{}\": {}", key, t_value);
        }
        out += "}";
        return formatter<std::string_view>::format(out, t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::sdk::ObjectInfo> : formatter<std::string_view> {
    auto format(const sgrn::sdk::ObjectInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("ObjectInfo{{id={}, name=\"{}\", status=\"{}\", metadata={}}}", t_info.id_,
                                                       t_info.name_, t_info.status_, sgrn::sdk::detail::jsonCompact(t_info.metadata_)),
            t_ctx);
    }
};
