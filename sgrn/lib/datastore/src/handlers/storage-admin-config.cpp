#include <sgrn/datastore/handlers/storage-admin.hpp>

#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/init/rate-limit.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/utils/system-config.hpp>
#include <sgrn/datastore/wire/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage-admin-internal.hpp"

namespace sgrn::datastore::handlers::storage_admin
{
using namespace drogon;
using namespace detail;

// ============================================================================
// System config (dashboard-tunable sgrn.json)
// ============================================================================

namespace
{

// Restart-required leaf paths changed since boot. In-memory by design: after
// a restart every saved value is live, so there is nothing pending anymore.
std::mutex g_pending_restart_mutex;
std::set<std::string> g_pending_restart;

std::vector<std::string> splitConfigPath(const std::string& t_dotted) {
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

const Json::Value* lookupPath(const Json::Value& t_root, const std::string& t_dotted) {
    const Json::Value* node = &t_root;
    for (const auto& part : splitConfigPath(t_dotted)) {
        if (!node->isObject() || !node->isMember(part)) {
            return nullptr;
        }
        node = &(*node)[part];
    }
    return node;
}

void assignPath(Json::Value& t_root, const std::string& t_dotted, const Json::Value& t_value) {
    Json::Value* node = &t_root;
    const auto parts = splitConfigPath(t_dotted);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        Json::Value& child = (*node)[parts[i]];
        if (!child.isObject()) {
            child = Json::Value(Json::objectValue);
        }
        node = &child;
    }
    (*node)[parts.back()] = t_value;
}

std::string readConfigFile(const std::string& t_path, Json::Value& o_root) {
    std::ifstream in(t_path, std::ios::binary);
    if (!in) {
        return "cannot open config file: " + t_path;
    }
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(raw.data(), raw.data() + raw.size(), &o_root, &errors)) {
        return "config file is not valid JSON: " + errors;
    }
    if (!o_root.isObject()) {
        return "config file root is not a JSON object";
    }
    return {};
}

// Backup current file to `<path>.bak`, then replace atomically via tmp +
// rename so a crash mid-save never leaves a half-written sgrn.json.
std::string atomicWriteConfig(const std::string& t_path, const Json::Value& t_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::copy_file(t_path, t_path + ".bak", fs::copy_options::overwrite_existing, ec);
    ec.clear();
    const std::string tmp = t_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "cannot write temp config file (check ownership/permissions): " + tmp;
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        out << Json::writeString(writer, t_root);
        out.flush();
        if (!out) {
            return "failed while writing temp config file: " + tmp;
        }
    }
    fs::rename(tmp, t_path, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return "atomic replace failed: " + ec.message();
    }
    return {};
}

std::string fieldTypeName(sgrn::datastore::sysconfig::FieldType t_type) {
    using sgrn::datastore::sysconfig::FieldType;
    switch (t_type) {
        case FieldType::UInt:
            return "uint";
        case FieldType::Double:
            return "double";
        case FieldType::String:
            return "string";
        case FieldType::Bool:
            return "bool";
        case FieldType::StringArray:
            return "string[]";
    }
    return "string";
}

Json::Value systemConfigSchemaJson() {
    Json::Value schema = Json::arrayValue;
    for (const auto& spec : sgrn::datastore::sysconfig::systemConfigSchema()) {
        Json::Value e;
        e["path"] = spec.path;
        e["type"] = fieldTypeName(spec.type);
        e["hot"] = spec.hot;
        e["section"] = spec.section;
        e["label"] = spec.label;
        if (spec.type == sgrn::datastore::sysconfig::FieldType::UInt) {
            e["min"] = Json::UInt64(spec.min_value);
            if (spec.max_value != 0) {
                e["max"] = Json::UInt64(spec.max_value);
            }
        }
        schema.append(std::move(e));
    }
    return schema;
}

// Dashboard values: exactly the schema leaves, read from the sanitized file
// (secrets can never appear here — sanitizeForAdmin redacts them first).
Json::Value systemConfigValuesJson(const Json::Value& t_sanitized_root) {
    Json::Value values = Json::Value(Json::objectValue);
    for (const auto& spec : sgrn::datastore::sysconfig::systemConfigSchema()) {
        if (const Json::Value* node = lookupPath(t_sanitized_root, spec.path)) {
            if (!node->isNull()) {
                assignPath(values, spec.path, *node);
            }
        }
    }
    return values;
}

bool startsWithPath(const std::string& t_path, const std::string& t_prefix) {
    return t_path.size() > t_prefix.size() && t_path.compare(0, t_prefix.size(), t_prefix) == 0 && t_path[t_prefix.size()] == '.';
}

struct SystemConfigResponse {
    std::string config_path;
    sgrn::json::RawJson values{"{}"};
    sgrn::json::RawJson schema{"[]"};
    sgrn::json::RawJson live{"{}"};
    std::vector<std::string> pending_restart;
};

struct SystemConfigUpdateResponse {
    std::string config_path;
    std::string backup_path;
    std::vector<std::string> applied_hot;
    std::vector<std::string> restart_required;
    std::vector<std::string> pending_restart;
    std::vector<std::string> warnings;
};

struct SystemConfigAuditDetails {
    std::vector<std::string> applied_hot;
    std::vector<std::string> restart_required;
    std::string config_path;
};

std::string compactJson(const Json::Value& t_value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, t_value);
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleGetSystemConfig(HttpRequestPtr tsp_req) {
    if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{"Forbidden: Platform administrator privileges required", k403Forbidden, "SystemConfig"});
    }
    try {
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }
        const auto live_s3 = sgrn::json::write(sgrn::datastore::services::storage::currentStorageConfig().toJsonConfig());
        if (live_s3.hasError()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Failed to serialize live storage configuration", k500InternalServerError, "SystemConfig"});
        }
        const std::string live_json = "{\"custom_config\":{\"s3\":" + live_s3.value() + "}}";
        // Effective-now values (live in-memory). Identical to `values` for
        // every hot key — PUT applies synchronously — and the source of truth
        // the upload path actually reads. Differs only if the file was
        // hand-edited without restarting for restart-required keys.
        std::vector<std::string> pending_restart;
        {
            std::lock_guard lock(g_pending_restart_mutex);
            pending_restart.assign(g_pending_restart.begin(), g_pending_restart.end());
        }
        const Json::Value values = systemConfigValuesJson(sgrn::datastore::sysconfig::sanitizeForAdmin(root));
        const Json::Value schema = systemConfigSchemaJson();
        co_return sgrn::createTypedResponse(tsp_req,
            SystemConfigResponse{path, sgrn::json::RawJson(compactJson(values)), sgrn::json::RawJson(compactJson(schema)),
                sgrn::json::RawJson(live_json), std::move(pending_restart)},
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "SystemConfig"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateSystemConfig(HttpRequestPtr tsp_req) {
    if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{"Forbidden: Platform administrator privileges required", k403Forbidden, "SystemConfig"});
    }
    try {
        auto req_res = parseTypedRequest<detail::UpdateSystemConfigRequest>(tsp_req);
        if (req_res.hasError() || req_res->values.str.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Expected JSON body {\"values\": {...}}", k400BadRequest, "SystemConfig"});
        }
        Json::CharReaderBuilder builder;
        Json::Value values_json;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const auto& val_str = req_res->values.str;
        if (!reader->parse(val_str.data(), val_str.data() + val_str.size(), &values_json, &errs) || !values_json.isObject()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Expected JSON body {\"values\": {...}}", k400BadRequest, "SystemConfig"});
        }
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }

        sgrn::datastore::sysconfig::ApplyReport report;
        if (const std::string err = sgrn::datastore::sysconfig::applyAdminUpdate(root, values_json, report); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "SystemConfig"});
        }
        if (const std::string err = atomicWriteConfig(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }

        // --- hot-apply -------------------------------------------------------
        // s3.*: republish the live StorageConfig (per-request snapshots pick
        // it up on the next upload — never mid-upload).
        bool touched_s3 = false;
        bool touched_rl = false;
        for (const auto& p : report.hot) {
            touched_s3 = touched_s3 || startsWithPath(p, "custom_config.s3");
            touched_rl = touched_rl || startsWithPath(p, "custom_config.rate_limiting");
        }
        for (const auto& p : report.restart) {
            touched_s3 = touched_s3 || startsWithPath(p, "custom_config.s3");
            touched_rl = touched_rl || startsWithPath(p, "custom_config.rate_limiting");
        }
        std::vector<std::string> applied_hot(report.hot.begin(), report.hot.end());
        std::vector<std::string> restart_required(report.restart.begin(), report.restart.end());
        if (touched_s3 && root.isMember("custom_config") && root["custom_config"].isMember("s3")) {
            sgrn::datastore::services::storage::publishStorageConfig(
                sgrn::datastore::services::storage::StorageConfig::fromS3Json(root["custom_config"]["s3"]));
        }
        if (touched_rl && root.isMember("custom_config")) {
            // No-op when the advice was never registered (disabled at boot):
            // report those paths as restart-required instead of pretending.
            if (sgrn::datastore::ratelimit::rateLimitHolder()->enabled_at_boot) {
                sgrn::datastore::ratelimit::publishRateLimitConfig(
                    sgrn::datastore::ratelimit::RateLimitConfig::fromJson(root["custom_config"]));
            } else {
                std::vector<std::string> still_hot;
                for (const auto& p : applied_hot) {
                    if (startsWithPath(p, "custom_config.rate_limiting")) {
                        restart_required.push_back(p);
                    } else {
                        still_hot.push_back(p);
                    }
                }
                applied_hot.swap(still_hot);
            }
        }

        {
            std::lock_guard lock(g_pending_restart_mutex);
            for (const auto& p : restart_required) {
                g_pending_restart.insert(p);
            }
        }
        std::vector<std::string> pending_restart;
        {
            std::lock_guard lock(g_pending_restart_mutex);
            pending_restart.assign(g_pending_restart.begin(), g_pending_restart.end());
        }
        std::vector<std::string> warnings(report.warnings.begin(), report.warnings.end());

        // --- Audit: system.config_updated ------------------------------------
        {
            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                std::string config_org;
                int32_t actor_id = 0;
                std::string actor_name;
                if (tsp_req->attributes()->find("auth_context")) {
                    const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
                    actor_id = auth.user.id;
                    actor_name = auth.user.email;
                    config_org = auth.user.organisation;
                } else if (tsp_req->attributes()->find("session_claims")) {
                    const auto& user = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
                    actor_id = user.id;
                    actor_name = user.email;
                    config_org = user.organisation;
                }
                co_await sgrn::datastore::audit::AuditLogger::log(db_res.value(), config_org, "user", actor_id, actor_name,
                    "system.config_updated", "system_config", path, tsp_req->getPeerAddr().toIp(),
                    SystemConfigAuditDetails{applied_hot, restart_required, path}, "success");
            }
        }

        co_return sgrn::createTypedResponse(tsp_req,
            SystemConfigUpdateResponse{
                path, path + ".bak", std::move(applied_hot), std::move(restart_required), std::move(pending_restart), std::move(warnings)},
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "SystemConfig"});
    }
}

} // namespace sgrn::datastore::handlers::storage_admin
