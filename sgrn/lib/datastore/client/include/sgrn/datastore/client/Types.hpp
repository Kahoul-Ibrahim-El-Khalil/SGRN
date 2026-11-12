#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <sgrn/utils/glaze.hpp>
#include <stdexcept>

namespace sgrn::datastore::client
{

enum class StorageScope {
    Auto,
    Personal,
    Users,
    AutomatedServices,
    Domain,
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
    std::string sha256{};
    std::string etag{};
    bool is_encrypted{false};
    std::string key_id{};
};

struct DriveListing {
    std::string path{};
    std::vector<DrivePathNode> trail{};
    std::vector<DriveFolderInfo> folders{};
    std::vector<DriveFileInfo> files{};
};

struct IdNamePair {
    std::string id_{};
    std::string name_{};
};

struct AdminUserEntry {
    int64_t id_{0};
    std::string email_{};
    std::string first_name_{};
    std::string family_name_{};
    std::string domain_{};
    std::string status_{};
};

/// Garage storage administration (admin-only endpoints).
struct StorageBucketCensus {
    std::string name_;
    int64_t garage_objects_{0};
    int64_t garage_bytes_{0};
    bool garage_truncated_{false};
    int64_t db_objects_{0};
    int64_t db_bytes_{0};
    int64_t db_files_{0};
};

struct StorageOrphanKey {
    std::string key_;
    int64_t size_{0};
    std::string etag_;
};

struct StorageOrphansReport {
    std::string bucket_;
    std::string prefix_;
    int64_t garage_scanned_{0};
    bool garage_truncated_{false};
    std::vector<StorageOrphanKey> garage_only_;
    int64_t garage_only_count_{0};
    int64_t garage_only_bytes_{0};
    std::vector<StorageOrphanKey> db_missing_;
    int64_t db_missing_unchecked_{0};
};

struct StoragePurgeResult {
    bool dry_run_{true};
    std::vector<std::string> affected_; // would_delete (dry run) or deleted
    std::vector<std::string> errors_;
    std::vector<std::string> skipped_;
};

struct ServiceEntry {
    int64_t id_{0};
    std::string name_{};
    std::string token_{};
    bool is_active_{false};
    std::string domain_{};
    std::string created_at_{};
};

struct NewUser {
    std::string first_name_;
    std::string family_name_;
    std::string email_;
    std::string password_;
    std::string phone_number_;
    std::string organisation_;
    std::string status_;
    std::string domain_;
};

struct NewService {
    std::string name_;
    std::string organisation_;
    std::string kind_;
    std::string domain_;
};

struct ServiceCredentials {
    std::string message_{};
    std::string token_{};
    std::string token_secret_{};
};

struct ObjectInfo {
    int32_t id_{};
    std::string name_{};
    std::string status_{};
    sgrn::json::Value metadata_{};
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
        case StorageScope::Domain:
            return "domain";
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

} // namespace detail

} // namespace sgrn::datastore::client

namespace sgrn::sdk
{
using namespace sgrn::datastore::client;
}

template <>
struct fmt::formatter<sgrn::datastore::client::ObjectInfo> : formatter<std::string_view> {
    auto format(const sgrn::datastore::client::ObjectInfo& t_info, format_context& t_ctx) const {
        auto metadata = sgrn::json::write(t_info.metadata_);
        return formatter<std::string_view>::format(fmt::format("ObjectInfo{{id={}, name=\"{}\", status=\"{}\", metadata={}}}", t_info.id_,
                                                       t_info.name_, t_info.status_, metadata.hasError() ? "{}" : metadata.value()),
            t_ctx);
    }
};
