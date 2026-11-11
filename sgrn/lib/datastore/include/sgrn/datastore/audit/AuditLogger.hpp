#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/utils/json-serialization.hpp>
#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::audit
{

struct AuditVerificationResult {
    bool is_valid{false};
    int64_t total_rows{0};
    std::optional<int64_t> first_broken_seq;
    std::optional<int64_t> first_broken_id;
    std::string failure_reason;
    int64_t verified_checkpoints{0};
};

struct AuditCheckpointResult {
    bool success{false};
    int64_t checkpoint_id{0};
    int64_t last_seq{0};
    std::string last_hash;
    std::string signature;
    std::string public_key;
    std::string error;
};

class AuditLogger {
public:
    // Serialize a typed details object with Glaze, then append the event to
    // core.audit_logs via core.append_audit_log().
    template <typename T>
    static drogon::Task<bool> log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation,
        std::string t_actor_type, // "user", "automated_service", "system"
        std::optional<int32_t> t_actor_id, std::string t_actor_name,
        std::string t_action,      // e.g. "auth.login", "user.created", "quota.updated", "audit.purged"
        std::string t_target_type, // "user", "automated_service", "role", "file", "organisation"
        std::string t_target_id, std::string t_ip, const T& t_details, std::string t_status = "success") { // "success", "failure"
        auto details_result = sgrn::json::write(t_details);
        if (details_result.hasError()) {
            co_return false;
        }
        co_return co_await logSerialized(std::move(tsp_db_client), std::move(t_organisation), std::move(t_actor_type), t_actor_id,
            std::move(t_actor_name), std::move(t_action), std::move(t_target_type), std::move(t_target_id), std::move(t_ip),
            std::move(details_result).value(), std::move(t_status));
    }

    // Compatibility boundary for legacy dynamic JsonCpp payloads. New call
    // sites should use the typed overload above.
    static drogon::Task<bool> log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
        std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type,
        std::string t_target_id, std::string t_ip, Json::Value t_details = Json::objectValue, std::string t_status = "success");

    // Append already serialized JSON. The input must be a valid JSON object.
    static drogon::Task<bool> logSerialized(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
        std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type,
        std::string t_target_id, std::string t_ip, std::string t_details_json, std::string t_status = "success");

    // Verify hash chain and checkpoints for an organisation (or across all organisations if empty)
    static drogon::Task<AuditVerificationResult> verifyChain(drogon::orm::DbClientPtr tsp_db_client, const std::string& t_organisation);

    // Create an Ed25519-signed checkpoint for an organisation
    static drogon::Task<AuditCheckpointResult> createCheckpoint(
        drogon::orm::DbClientPtr tsp_db_client, const std::string& t_organisation, const std::string& t_sink_target = "local");

    // Pure canonical hash computation for test and client verification
    static std::string computeRowHash(const std::string& prev_hash, int64_t seq, const std::string& org, const std::string& actor_type,
        const std::optional<int32_t>& actor_id, const std::string& actor_name, const std::string& action, const std::string& target_type,
        const std::string& target_id, const std::string& ip, const Json::Value& details, const std::string& status,
        const std::string& timestamp_iso);

    // Ed25519 cryptographic routines
    static bool generateEd25519KeyPair(std::string& out_priv_hex, std::string& out_pub_hex);
    static bool signEd25519(const std::string& message, const std::string& priv_hex, std::string& out_sig_hex);
    static bool verifyEd25519(const std::string& message, const std::string& sig_hex, const std::string& pub_hex);
};

// Global CLI helper for --verify-audit
int verifyAuditTrail(const std::string& t_organisation = "");

} // namespace sgrn::datastore::audit
