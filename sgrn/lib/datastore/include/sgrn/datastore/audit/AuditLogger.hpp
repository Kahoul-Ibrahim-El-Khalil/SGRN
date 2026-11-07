#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
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
    // Asynchronously log an event to core.audit_logs table via core.append_audit_log()
    static drogon::Task<bool> log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation,
        std::string t_actor_type, // "user", "automated_service", "system"
        std::optional<int32_t> t_actor_id, std::string t_actor_name,
        std::string t_action,      // e.g. "auth.login", "user.created", "quota.updated", "audit.purged"
        std::string t_target_type, // "user", "automated_service", "role", "file", "organisation"
        std::string t_target_id, std::string t_ip, Json::Value t_details = Json::objectValue,
        std::string t_status = "success" // "success", "failure"
    );

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
