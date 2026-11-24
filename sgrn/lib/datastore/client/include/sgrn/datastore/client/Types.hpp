#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <sgrn/datastore/wire/admin.hpp>
#include <sgrn/datastore/wire/storage.hpp>
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

// The public client types ARE the shared wire types. They are plain Glaze
// aggregates, so a response can be decoded straight into them with no
// mirror struct and no field-by-field converter.
using DrivePathNode = ::sgrn::datastore::storage::DrivePathNode;
using DriveFolderInfo = ::sgrn::datastore::storage::DriveFolderInfo;
using DriveFileInfo = ::sgrn::datastore::storage::DriveFileInfo;
using DriveListing = ::sgrn::datastore::storage::DriveListing;

using StorageBucketCensus = ::sgrn::datastore::storage::StorageBucketCensus;
using StorageOrphanKey = ::sgrn::datastore::storage::StorageOrphanKey;
using StorageOrphansReport = ::sgrn::datastore::storage::StorageOrphansReport;
using ObjectInfo = ::sgrn::datastore::storage::ObjectInfo;

using FileVersionInfo = ::sgrn::datastore::storage::FileVersionInfo;
using FileVersionRestoreRequest = ::sgrn::datastore::storage::FileVersionRestoreRequest;
using FileVersionRestoreReceipt = ::sgrn::datastore::storage::FileVersionRestoreReceipt;
using FileVersionTrimReceipt = ::sgrn::datastore::storage::FileVersionTrimReceipt;

using AdminUserEntry = ::sgrn::datastore::admin::AdminUserEntry;
using ServiceEntry = ::sgrn::datastore::admin::ServiceEntry;
using NewUser = ::sgrn::datastore::admin::NewUser;
using NewService = ::sgrn::datastore::admin::NewService;
using ServiceCredentials = ::sgrn::datastore::admin::ServiceCredentials;

/// {id, name} list entry. Ids are kept as strings because the generated CRUD
/// views may stringify them.
struct IdNamePair {
    std::string id{};
    std::string name{};
};

/// Normalised result of either purge response shape (dry run or real).
struct StoragePurgeResult {
    bool dry_run{true};
    std::vector<std::string> affected; // would_delete (dry run) or deleted
    std::vector<std::string> errors;
    std::vector<std::string> skipped;
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
