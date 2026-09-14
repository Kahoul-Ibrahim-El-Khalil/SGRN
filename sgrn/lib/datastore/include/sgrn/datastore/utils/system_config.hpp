// sgrn/datastore/utils/system_config.hpp
//
// Admin-tunable system configuration: schema, sanitization, validation.
//
// The dashboard edits sgrn.json through the admin API instead of SSH. This
// header owns everything the endpoint needs that is pure (no Drogon app
// singleton, no I/O), so it stays unit-testable:
//
//   - systemConfigSchema(): every tunable leaf, its type, and whether a
//     change applies live (hot) or needs a process restart.
//   - Secrets policy: any leaf whose final segment is a known secret name
//     (passwd, secret_key, ...) is neither readable nor writable via the
//     API. Sections that structurally embed secrets (db_clients,
//     redis_clients, plugins, listeners) are file-only in v1.
//   - applyAdminUpdate(): validates a nested partial update against the
//     schema plus semantic rules (S3 5MB part floor, chunking band below
//     the max), then deep-merges it into the file root in place.
//
// Env placeholders ("${...}") are opaque strings throughout: parsing and
// serialization never resolve them, so round-trips preserve them byte-wise
// as values (whitespace/formatting of the file itself is normalized).
#pragma once

#include <cstdint>
#include <json/json.h>
#include <string>
#include <vector>

namespace sgrn::datastore::sysconfig
{

enum class FieldType { UInt, Double, String, Bool, StringArray };

struct FieldSpec {
    std::string path; // dotted from the sgrn.json root, e.g. "custom_config.s3.chunking_threshold_mb"
    FieldType type = FieldType::UInt;
    bool hot = false;    // true = applied live; false = restart required
    std::string section; // UI grouping
    std::string label;
    uint64_t min_value = 0;  // UInt only
    uint64_t max_value = 0;  // UInt only, 0 = no cap
    double min_double = 0.0; // Double only
    double max_double = 0.0; // Double only, 0 = no cap
};

// Every leaf the admin API accepts. Unknown paths are rejected (fail closed).
const std::vector<FieldSpec>& systemConfigSchema();

// True when the dotted path addresses a secret (also true for anything
// under a secret-bearing array, matched by final segment).
bool isSecretPath(const std::string& t_dotted);

// True when the dotted path is exactly one schema entry.
bool isKnownPath(const std::string& t_dotted);

// Deep copy of t_root with every secret leaf replaced by "***".
// Never throws on well-formed input; unknown shapes pass through untouched.
Json::Value sanitizeForAdmin(const Json::Value& t_root);

struct ApplyReport {
    std::vector<std::string> hot;      // changed leaf paths applied without restart
    std::vector<std::string> restart;  // changed leaf paths needing a restart
    std::vector<std::string> warnings; // non-fatal notes (e.g. clamps)
};

// Validate t_update (nested partial mirroring the file shape) and, on
// success, deep-merge it into t_file_root in place. Returns an empty string
// on success, otherwise a human-readable rejection reason and t_file_root
// is left unmodified.
std::string applyAdminUpdate(Json::Value& t_file_root, const Json::Value& t_update, ApplyReport& o_report);

// Formats-registry validation (admin CRUD). Pure string rules mirroring the
// storage.formats constraints: extension is lower(extension) + varchar(15),
// mime_type is type/subtype + varchar(128). Empty string = valid.
std::string validateFormatExtension(const std::string& t_extension);
std::string validateMimeType(const std::string& t_mime);

} // namespace sgrn::datastore::sysconfig
