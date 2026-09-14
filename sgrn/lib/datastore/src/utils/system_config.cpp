// sgrn/datastore/utils/system_config.cpp — schema, sanitization, validation.
// See the header for the contract. Pure Json logic: no Drogon, no I/O.

#include <sgrn/datastore/utils/system_config.hpp>

#include <algorithm>
#include <sstream>

namespace sgrn::datastore::sysconfig
{
namespace
{

// Leaf names that always carry secrets, wherever they appear (covers
// db_clients[i].passwd, plugins[].secret_key, app.session.cookie_key, ...).
bool isSecretLeaf(std::string_view t_leaf) {
    return t_leaf == "passwd" || t_leaf == "secret_key" || t_leaf == "access_key" || t_leaf == "secret" || t_leaf == "token" ||
           t_leaf == "token_secret" || t_leaf == "cookie_key" || t_leaf == "jwt_secret";
}

std::vector<std::string> splitDotted(const std::string& t_dotted) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t_dotted) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

const FieldSpec* findSpec(const std::string& t_dotted) {
    for (const auto& spec : systemConfigSchema()) {
        if (spec.path == t_dotted) {
            return &spec;
        }
    }
    return nullptr;
}

// Recursively redact secret leaves in place.
void redactSecrets(Json::Value& t_node) {
    if (t_node.isObject()) {
        for (const auto& key : t_node.getMemberNames()) {
            if (isSecretLeaf(key)) {
                t_node[key] = "***";
            } else {
                redactSecrets(t_node[key]);
            }
        }
    } else if (t_node.isArray()) {
        for (auto& item : t_node) {
            redactSecrets(item);
        }
    }
}

bool isUIntLike(const Json::Value& t_v) {
    if (t_v.isUInt() || t_v.isUInt64()) {
        return true;
    }
    // jsoncpp parses small non-negative ints as Int; accept those too.
    return t_v.isInt() && t_v.asInt() >= 0;
}

uint64_t asUInt(const Json::Value& t_v) {
    return t_v.isUInt64() ? t_v.asUInt64() : static_cast<uint64_t>(t_v.asUInt());
}

bool isDoubleLike(const Json::Value& t_v) {
    return t_v.isDouble() || isUIntLike(t_v);
}

// Collect dotted leaf paths of t_update under t_prefix (arrays count as one
// leaf — only StringArray fields use them).
void collectLeaves(
    const Json::Value& t_update, const std::string& t_prefix, std::vector<std::pair<std::string, const Json::Value*>>& o_out) {
    if (!t_update.isObject()) {
        return;
    }
    for (const auto& key : t_update.getMemberNames()) {
        const std::string dotted = t_prefix.empty() ? key : t_prefix + "." + key;
        const Json::Value& child = t_update[key];
        if (child.isObject()) {
            collectLeaves(child, dotted, o_out);
        } else {
            o_out.emplace_back(dotted, &child);
        }
    }
}

// Navigate/create the parent object of t_parts inside t_root. Returns null
// when an intermediate segment exists but is not an object.
Json::Value* navigateParent(Json::Value& t_root, const std::vector<std::string>& t_parts) {
    Json::Value* node = &t_root;
    for (size_t i = 0; i + 1 < t_parts.size(); ++i) {
        Json::Value& child = (*node)[t_parts[i]];
        if (child.isNull()) {
            child = Json::Value(Json::objectValue);
        }
        if (!child.isObject()) {
            return nullptr;
        }
        node = &child;
    }
    return node;
}

std::string checkType(const FieldSpec& t_spec, const Json::Value& t_v) {
    switch (t_spec.type) {
        case FieldType::UInt:
            if (!isUIntLike(t_v)) {
                return "expected a non-negative integer";
            }
            if (asUInt(t_v) < t_spec.min_value) {
                return "below minimum " + std::to_string(t_spec.min_value);
            }
            if (t_spec.max_value != 0 && asUInt(t_v) > t_spec.max_value) {
                return "above maximum " + std::to_string(t_spec.max_value);
            }
            return {};
        case FieldType::Double: {
            if (!isDoubleLike(t_v)) {
                return "expected a number";
            }
            const double d = t_v.asDouble();
            if (d < t_spec.min_double) {
                return "below minimum";
            }
            if (t_spec.max_double != 0.0 && d > t_spec.max_double) {
                return "above maximum";
            }
            return {};
        }
        case FieldType::String:
            return t_v.isString() ? std::string{} : "expected a string";
        case FieldType::Bool:
            return t_v.isBool() ? std::string{} : "expected a boolean";
        case FieldType::StringArray: {
            if (!t_v.isArray()) {
                return "expected an array of strings";
            }
            for (const auto& item : t_v) {
                if (!item.isString()) {
                    return "expected an array of strings";
                }
            }
            return {};
        }
    }
    return "unknown field type";
}

} // namespace

const std::vector<FieldSpec>& systemConfigSchema() {
    // S3 plugin worker pool / endpoint / keys live in `plugins` (file-only:
    // structural + secret-bearing). Everything here is hot except where the
    // consumer snapshots at startup (rate limiting `enabled` flip — the
    // pre-routing advice is only registered when enabled at boot).
    static const std::vector<FieldSpec> kSchema = {
        // --- Storage / S3 (custom_config.s3): all hot, read per request ---
        {"custom_config.s3.default_bucket", FieldType::String, true, "Storage / S3", "Default bucket", 0, 0},
        {"custom_config.s3.worker_threads", FieldType::UInt, false, "Storage / S3", "S3 worker threads", 1, 64},
        {"custom_config.s3.threshold_compress_ram_mb", FieldType::UInt, true, "Storage / S3", "In-RAM compress threshold (MB)", 0, 4096},
        {"custom_config.s3.threshold_presigned_mb", FieldType::UInt, true, "Storage / S3", "Presigned-URL threshold (MB)", 1, 2048},
        {"custom_config.s3.max_file_size_mb", FieldType::UInt, true, "Storage / S3", "Max file size (MB)", 1, 8192},
        {"custom_config.s3.default_expiration_seconds", FieldType::UInt, true, "Storage / S3", "Presigned URL expiry (s)", 60, 86400},
        {"custom_config.s3.compression_size_threshold_kb", FieldType::UInt, true, "Storage / S3", "Compress files at/above (KB)", 0,
            1048576},
        {"custom_config.s3.compression_level", FieldType::UInt, true, "Storage / S3", "Compression level (1-22)", 1, 22},
        {"custom_config.s3.allowed_extensions", FieldType::StringArray, true, "Storage / S3", "Allowed extensions"},
        {"custom_config.s3.prohibited_extensions", FieldType::StringArray, true, "Storage / S3", "Prohibited extensions"},
        // Low floor (1MB) is deliberate: a single-part multipart upload is
        // legal S3/MinIO (the 5MB minimum only binds non-final parts), so
        // admins can force the multipart path with small files to debug it.
        // Compared against the ORIGINAL file size (pre-compression) — the
        // number the admin sees — same basis as max_file_size.
        {"custom_config.s3.chunking_threshold_mb", FieldType::UInt, true, "Storage / S3", "Multipart threshold (MB of original size)", 1,
            8192},
        {"custom_config.s3.chunk_part_size_mb", FieldType::UInt, true, "Storage / S3", "Multipart part size (MB, min 5)", 5, 512},
        // --- Rate limiting (custom_config.rate_limiting): hot via holder swap ---
        {"custom_config.rate_limiting.enabled", FieldType::Bool, false, "Rate limiting", "Enabled (toggle needs restart)"},
        {"custom_config.rate_limiting.auth_endpoint_limit", FieldType::UInt, true, "Rate limiting", "Auth limit", 1, 100000},
        {"custom_config.rate_limiting.auth_endpoint_window_s", FieldType::UInt, true, "Rate limiting", "Auth window (s)", 1, 3600},
        {"custom_config.rate_limiting.general_endpoint_limit", FieldType::UInt, true, "Rate limiting", "General limit", 1, 100000},
        {"custom_config.rate_limiting.general_endpoint_window_s", FieldType::UInt, true, "Rate limiting", "General window (s)", 1, 3600},
        {"custom_config.rate_limiting.storage_endpoint_limit", FieldType::UInt, true, "Rate limiting", "Storage limit", 1, 100000},
        {"custom_config.rate_limiting.storage_endpoint_window_s", FieldType::UInt, true, "Rate limiting", "Storage window (s)", 1, 3600},
        {"custom_config.rate_limiting.page_endpoint_limit", FieldType::UInt, true, "Rate limiting", "Page limit", 1, 100000},
        {"custom_config.rate_limiting.page_endpoint_window_s", FieldType::UInt, true, "Rate limiting", "Page window (s)", 1, 3600},
        {"custom_config.rate_limiting.burst_allowance", FieldType::UInt, true, "Rate limiting", "Burst allowance", 0, 10000},
        // --- Telemetry (custom_config.telemetry): restart (no live consumer yet) ---
        {"custom_config.telemetry.batching_enabled", FieldType::Bool, false, "Telemetry", "Batching enabled"},
        {"custom_config.telemetry.acknowledge_on_queue", FieldType::Bool, false, "Telemetry", "Acknowledge on queue"},
        {"custom_config.telemetry.flush_interval_s", FieldType::Double, false, "Telemetry", "Flush interval (s)", 0, 0, 0.1, 3600.0},
        {"custom_config.telemetry.max_batch_size", FieldType::UInt, false, "Telemetry", "Max batch size", 1, 100000},
        {"custom_config.telemetry.max_pending_items", FieldType::UInt, false, "Telemetry", "Max pending items", 1, 1000000},
        // --- Server (app.*): restart (drogon listeners are immutable at runtime) ---
        {"app.client_max_body_size", FieldType::UInt, false, "Server", "Max request body (bytes)", 1048576, 8589934592ULL},
        {"app.max_connections", FieldType::UInt, false, "Server", "Max connections", 1, 100000},
        {"app.max_memory", FieldType::UInt, false, "Server", "Max memory (bytes)", 67108864, 68719476736ULL},
        {"app.threads_num", FieldType::UInt, false, "Server", "I/O threads", 1, 64},
        {"app.log_level", FieldType::String, false, "Server", "Log level"},
        {"app.enable_access_log", FieldType::Bool, false, "Server", "Access log"},
    };
    return kSchema;
}

bool isSecretPath(const std::string& t_dotted) {
    const auto parts = splitDotted(t_dotted);
    if (parts.empty()) {
        return false;
    }
    return isSecretLeaf(parts.back());
}

bool isKnownPath(const std::string& t_dotted) {
    return findSpec(t_dotted) != nullptr;
}

Json::Value sanitizeForAdmin(const Json::Value& t_root) {
    Json::Value copy = t_root;
    redactSecrets(copy);
    return copy;
}

std::string applyAdminUpdate(Json::Value& t_file_root, const Json::Value& t_update, ApplyReport& o_report) {
    o_report = ApplyReport{};
    if (!t_update.isObject()) {
        return "body must be a JSON object";
    }
    if (!t_file_root.isObject()) {
        return "existing config is not a JSON object — refusing to merge";
    }

    std::vector<std::pair<std::string, const Json::Value*>> leaves;
    collectLeaves(t_update, "", leaves);
    if (leaves.empty()) {
        return "no settings supplied";
    }

    // --- validate everything before touching the file root ---
    for (const auto& [dotted, value] : leaves) {
        if (isSecretPath(dotted)) {
            return "refusing secret-bearing key '" + dotted + "' — edit the file directly for secrets";
        }
        const FieldSpec* spec = findSpec(dotted);
        if (spec == nullptr) {
            return "unknown setting '" + dotted + "' — file-only or misspelled";
        }
        const std::string type_err = checkType(*spec, *value);
        if (!type_err.empty()) {
            return "'" + dotted + "': " + type_err;
        }
        if (spec->type == FieldType::String && dotted == "app.log_level") {
            const std::string lvl = value->asString();
            if (lvl != "TRACE" && lvl != "DEBUG" && lvl != "INFO" && lvl != "WARN" && lvl != "ERROR" && lvl != "FATAL") {
                return "'app.log_level': expected one of TRACE/DEBUG/INFO/WARN/ERROR/FATAL";
            }
        }
    }

    // Work on a copy so semantic failures leave the caller's root untouched.
    Json::Value merged = t_file_root;
    for (const auto& [dotted, value] : leaves) {
        const auto parts = splitDotted(dotted);
        Json::Value* parent = navigateParent(merged, parts);
        if (parent == nullptr) {
            return "conflicting shape at '" + dotted + "' — an intermediate key is not an object";
        }
        (*parent)[parts.back()] = *value;
    }

    // --- semantic rules on the merged s3 block ---
    if (merged.isMember("custom_config") && merged["custom_config"].isObject() && merged["custom_config"].isMember("s3")) {
        const Json::Value& s3 = merged["custom_config"]["s3"];
        // Hard floor: S3/MinIO reject parts < 5MB (all but last). Clamp like
        // loadFromConfig() instead of failing the save.
        if (s3.isMember("chunk_part_size_mb") && isUIntLike(s3["chunk_part_size_mb"]) && asUInt(s3["chunk_part_size_mb"]) < 5) {
            merged["custom_config"]["s3"]["chunk_part_size_mb"] = Json::UInt64(5);
            o_report.warnings.push_back("custom_config.s3.chunk_part_size_mb clamped to the 5MB S3 minimum");
        }
        // The multipart band must exist: threshold below the max, or large
        // uploads are rejected by validateFileSize before multipart runs.
        if (s3.isMember("chunking_threshold_mb") && s3.isMember("max_file_size_mb") && isUIntLike(s3["chunking_threshold_mb"]) &&
            isUIntLike(s3["max_file_size_mb"]) && asUInt(s3["chunking_threshold_mb"]) >= asUInt(s3["max_file_size_mb"])) {
            return "custom_config.s3.chunking_threshold_mb must be below max_file_size_mb, or multipart is unreachable";
        }
    }

    t_file_root.swap(merged);
    for (const auto& [dotted, value] : leaves) {
        const FieldSpec* spec = findSpec(dotted);
        if (spec != nullptr && spec->hot) {
            o_report.hot.push_back(dotted);
        } else {
            o_report.restart.push_back(dotted);
        }
    }
    std::sort(o_report.hot.begin(), o_report.hot.end());
    std::sort(o_report.restart.begin(), o_report.restart.end());
    return {};
}

std::string validateFormatExtension(const std::string& t_extension) {
    if (t_extension.empty()) {
        return "extension is required";
    }
    if (t_extension.size() > 15) {
        return "extension exceeds the 15-character limit";
    }
    for (char c : t_extension) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return "extension must be lowercase alphanumeric plus ._-";
        }
    }
    return {};
}

std::string validateMimeType(const std::string& t_mime) {
    if (t_mime.empty()) {
        return "mime_type is required";
    }
    if (t_mime.size() > 128) {
        return "mime_type exceeds the 128-character limit";
    }
    const auto slash = t_mime.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= t_mime.size()) {
        return "mime_type must look like type/subtype";
    }
    if (t_mime.find(' ') != std::string::npos) {
        return "mime_type must not contain spaces";
    }
    return {};
}

} // namespace sgrn::datastore::sysconfig
