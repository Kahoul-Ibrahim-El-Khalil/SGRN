#include <sgrn/datastore/audit/AuditLogger.hpp>

#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/utils/crypto.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>

#include <drogon/HttpAppFramework.h>
#include <fmt/core.h>
#include <trantor/utils/Logger.h>

namespace sgrn::datastore::audit
{
namespace
{

struct AuditChainVerificationRow {
    bool is_valid{false};
    int64_t total_rows{0};
    std::optional<int64_t> first_broken_seq;
    std::optional<int64_t> first_broken_id;
    std::string failure_reason;
};

struct AuditCheckpointRow {
    int64_t id{0};
    int64_t last_seq{0};
    std::string last_hash;
    std::optional<std::string> signature;
    std::optional<std::string> public_key;
};

struct LatestAuditRow {
    int64_t seq{0};
    std::string row_hash;
};

struct CheckpointIdRow {
    int64_t id{0};
};

struct OrganisationNameRow {
    std::string organisation;
};

static constexpr char kOrganisationsQuery[] = "SELECT DISTINCT organisation FROM core.audit_logs ORDER BY organisation";
static constexpr char kVerifyChainQuery[] = "SELECT is_valid, total_rows, first_broken_seq, first_broken_id, failure_reason "
                                            "FROM core.verify_audit_chain($1::text)";
static constexpr char kCheckpointsQuery[] = "SELECT id, last_seq, last_hash, signature, public_key "
                                            "FROM core.audit_checkpoints WHERE organisation = $1::text ORDER BY last_seq ASC";
static constexpr char kLatestAuditQuery[] =
    "SELECT seq, row_hash FROM core.audit_logs WHERE organisation = $1::text ORDER BY seq DESC LIMIT 1";
static constexpr char kInsertCheckpointQuery[] =
    "INSERT INTO core.audit_checkpoints (organisation, last_seq, last_hash, sink_target, signature, public_key) "
    "VALUES ($1::text, $2::bigint, $3::text, $4::text, $5::text, $6::text) "
    "RETURNING id";

} // namespace

drogon::Task<bool> AuditLogger::logSerialized(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
    std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type, std::string t_target_id,
    std::string t_ip, std::string t_details_json, std::string t_status) {
    try {
        if (!tsp_db_client) {
            tsp_db_client = drogon::app().getDbClient();
        }
        if (!tsp_db_client) {
            co_return false;
        }

        // Insert atomically via core.append_audit_log() under advisory lock
        co_await tsp_db_client->execSqlCoro(
            "SELECT out_id, out_seq, out_prev_hash, out_row_hash FROM core.append_audit_log("
            "$1::text, $2::text, $3::int, $4::text, $5::text, $6::text, $7::text, $8::inet, $9::jsonb, $10::text)",
            t_organisation, t_actor_type, t_actor_id, t_actor_name, t_action,
            t_target_type.empty() ? std::optional<std::string>{} : t_target_type,
            t_target_id.empty() ? std::optional<std::string>{} : t_target_id, t_ip.empty() ? "127.0.0.1" : t_ip, t_details_json, t_status);

        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "AuditLogger failed to append audit record '" << t_action << "': " << e.what();
        co_return false;
    }
}

drogon::Task<bool> AuditLogger::log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
    std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type, std::string t_target_id,
    std::string t_ip, Json::Value t_details, std::string t_status) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string details_json = Json::writeString(writer, t_details);
    co_return co_await logSerialized(std::move(tsp_db_client), std::move(t_organisation), std::move(t_actor_type), t_actor_id,
        std::move(t_actor_name), std::move(t_action), std::move(t_target_type), std::move(t_target_id), std::move(t_ip),
        std::move(details_json), std::move(t_status));
}

drogon::Task<AuditVerificationResult> AuditLogger::verifyChain(drogon::orm::DbClientPtr tsp_db_client, const std::string& t_organisation) {
    AuditVerificationResult result;
    result.is_valid = false;

    if (!tsp_db_client) {
        tsp_db_client = drogon::app().getDbClient();
    }
    if (!tsp_db_client) {
        result.failure_reason = "Database client is not available";
        co_return result;
    }

    try {
        std::vector<std::string> orgs_to_verify;
        if (!t_organisation.empty()) {
            orgs_to_verify.push_back(t_organisation);
        } else {
            auto org_res = co_await tsp_db_client->execSqlCoro(kOrganisationsQuery);
            const auto org_rows = sgrn::orm::fromResult<OrganisationNameRow, kOrganisationsQuery>(org_res);
            for (const auto& row : org_rows) {
                orgs_to_verify.push_back(row.organisation);
            }
        }

        if (orgs_to_verify.empty()) {
            result.is_valid = true;
            result.failure_reason = "No audit log entries found";
            co_return result;
        }

        for (const auto& org : orgs_to_verify) {
            auto verify_res = co_await tsp_db_client->execSqlCoro(kVerifyChainQuery, org);

            if (verify_res.empty()) {
                result.is_valid = false;
                result.failure_reason = "core.verify_audit_chain returned no rows for org: " + org;
                co_return result;
            }

            const auto row = sgrn::orm::fromRow<AuditChainVerificationRow, kVerifyChainQuery>(verify_res[0]);
            result.total_rows += row.total_rows;

            if (!row.is_valid) {
                result.is_valid = false;
                result.first_broken_seq = row.first_broken_seq;
                result.first_broken_id = row.first_broken_id;
                result.failure_reason = "Org '" + org + "': " + row.failure_reason;
                co_return result;
            }

            // Verify checkpoints for this org
            auto chk_res = co_await tsp_db_client->execSqlCoro(kCheckpointsQuery, org);

            const auto checkpoints = sgrn::orm::fromResult<AuditCheckpointRow, kCheckpointsQuery>(chk_res);
            for (const auto& checkpoint : checkpoints) {
                if (!checkpoint.signature.has_value() || !checkpoint.public_key.has_value()) {
                    continue;
                }

                const std::string checkpoint_payload = org + ":" + std::to_string(checkpoint.last_seq) + ":" + checkpoint.last_hash;
                if (!verifyEd25519(checkpoint_payload, *checkpoint.signature, *checkpoint.public_key)) {
                    result.is_valid = false;
                    result.first_broken_seq = checkpoint.last_seq;
                    result.failure_reason =
                        "Checkpoint Ed25519 signature verification failed for checkpoint ID " + std::to_string(checkpoint.id);
                    co_return result;
                }
                ++result.verified_checkpoints;
            }
        }

        result.is_valid = true;
        result.failure_reason = "All audit log chains and checkpoints verified successfully";
        co_return result;
    } catch (const std::exception& e) {
        result.is_valid = false;
        result.failure_reason = std::string("Verification error: ") + e.what();
        co_return result;
    }
}

drogon::Task<AuditCheckpointResult> AuditLogger::createCheckpoint(
    drogon::orm::DbClientPtr tsp_db_client, const std::string& t_organisation, const std::string& t_sink_target) {
    AuditCheckpointResult result;
    result.success = false;

    if (!tsp_db_client) {
        tsp_db_client = drogon::app().getDbClient();
    }
    if (!tsp_db_client) {
        result.error = "Database client is not available";
        co_return result;
    }

    try {
        // Find latest audit record for organisation
        auto latest_res = co_await tsp_db_client->execSqlCoro(kLatestAuditQuery, t_organisation);

        if (latest_res.empty()) {
            result.error = "No audit log records exist for organisation: " + t_organisation;
            co_return result;
        }

        const auto latest = sgrn::orm::fromRow<LatestAuditRow, kLatestAuditQuery>(latest_res[0]);
        const int64_t last_seq = latest.seq;
        const std::string& last_hash = latest.row_hash;

        std::string priv_hex;
        std::string pub_hex;
        if (!generateEd25519KeyPair(priv_hex, pub_hex)) {
            result.error = "Failed to generate Ed25519 key pair for audit checkpoint";
            co_return result;
        }

        const std::string checkpoint_payload = t_organisation + ":" + std::to_string(last_seq) + ":" + last_hash;
        std::string sig_hex;
        if (!signEd25519(checkpoint_payload, priv_hex, sig_hex)) {
            result.error = "Failed to sign checkpoint with Ed25519";
            co_return result;
        }

        auto ins_res = co_await tsp_db_client->execSqlCoro(
            kInsertCheckpointQuery, t_organisation, last_seq, last_hash, t_sink_target, sig_hex, pub_hex);

        result.success = true;
        result.checkpoint_id = sgrn::orm::fromRowByName<CheckpointIdRow>(ins_res[0]).id;
        result.last_seq = last_seq;
        result.last_hash = last_hash;
        result.signature = sig_hex;
        result.public_key = pub_hex;
        co_return result;
    } catch (const std::exception& e) {
        result.error = std::string("Checkpoint creation failed: ") + e.what();
        co_return result;
    }
}

std::string AuditLogger::computeRowHash(const std::string& prev_hash, int64_t seq, const std::string& org, const std::string& actor_type,
    const std::optional<int32_t>& actor_id, const std::string& actor_name, const std::string& action, const std::string& target_type,
    const std::string& target_id, const std::string& ip, const Json::Value& details, const std::string& status,
    const std::string& timestamp_iso) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string details_str = Json::writeString(wb, details);

    const std::string canon = prev_hash + "|" + std::to_string(seq) + "|" + org + "|" + actor_type + "|" +
                              (actor_id.has_value() ? std::to_string(*actor_id) : "") + "|" + actor_name + "|" + action + "|" +
                              target_type + "|" + target_id + "|" + ip + "|" + details_str + "|" + status + "|" + timestamp_iso;

    auto hash_res = sgrn::utils::computeSha256Data(canon, sgrn::utils::HashEncoding::hex);
    return hash_res.hasValue() ? hash_res.value() : "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Ed25519 Cryptographic Implementation (delegated to sgrn::utils::crypto)
// ─────────────────────────────────────────────────────────────────────────────

bool AuditLogger::generateEd25519KeyPair(std::string& out_priv_hex, std::string& out_pub_hex) {
    return sgrn::utils::crypto::generateEd25519KeyPair(out_priv_hex, out_pub_hex);
}

bool AuditLogger::signEd25519(const std::string& message, const std::string& priv_hex, std::string& out_sig_hex) {
    return sgrn::utils::crypto::signEd25519(message, priv_hex, out_sig_hex);
}

bool AuditLogger::verifyEd25519(const std::string& message, const std::string& sig_hex, const std::string& pub_hex) {
    return sgrn::utils::crypto::verifyEd25519(message, sig_hex, pub_hex);
}

int verifyAuditTrail(const std::string& t_organisation) {
    auto db = drogon::app().getDbClient();
    if (!db) {
        fmt::print(stderr, "error: Database client not initialized\n");
        return 1;
    }

    auto res = drogon::sync_wait(AuditLogger::verifyChain(db, t_organisation));
    if (res.is_valid) {
        fmt::print("audit verify: OK ({} rows verified, {} signed checkpoints valid)\n", res.total_rows, res.verified_checkpoints);
        return 0;
    } else {
        fmt::print(stderr, "audit verify: FAILED\n");
        if (res.first_broken_seq.has_value()) {
            fmt::print(stderr, "  First broken sequence : {}\n", *res.first_broken_seq);
        }
        if (res.first_broken_id.has_value()) {
            fmt::print(stderr, "  First broken row ID   : {}\n", *res.first_broken_id);
        }
        fmt::print(stderr, "  Failure reason        : {}\n", res.failure_reason);
        return 2;
    }
}

} // namespace sgrn::datastore::audit
