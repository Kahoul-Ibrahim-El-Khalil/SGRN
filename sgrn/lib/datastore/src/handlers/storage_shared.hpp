// storage_shared.hpp — declarations shared across the storage.*.cpp split
// files (storage.cpp, storage_files.cpp, storage_drive.cpp).
//
// The upload-target path helpers below are defined in storage_files.cpp but
// are also called from the drive/directory handlers, so they need a
// declaration in one place. NOT part of the public handler interface —
// never include from outside sgrn/lib/datastore/src/handlers/.
#pragma once

#include <drogon/utils/Utilities.h> // drogon::SafeStringMap

#include <optional>
#include <string>

namespace sgrn::datastore::handlers::storage
{
// Normalize a query/form path into a canonical absolute virtual path
// ("/" for empty, collapsed leading slashes). Returns nullopt for invalid input.
std::optional<std::string> normalizePath(std::string t_path);

// Join a request "dir" with the relative file part into an upload target path.
std::string joinUploadTargetPath(std::string t_base_dir, std::string t_rel_name);

// Resolve the upload target for a single-file request: an explicit "path"
// form field wins over dir+rel-name joining. Returns nullopt (with
// t_out_error set) when the resolved path is invalid.
std::optional<std::string> resolveSingleUploadTarget(const drogon::SafeStringMap<std::string>& t_form_params,
    const std::string& t_query_dir, const std::string& t_rel_name, std::string& t_out_error);
} // namespace sgrn::datastore::handlers::storage
