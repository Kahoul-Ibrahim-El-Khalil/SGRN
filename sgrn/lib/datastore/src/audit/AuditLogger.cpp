#include <sgrn/datastore/audit/AuditLogger.hpp>

#include <sgrn/utils/crypto.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>

#include <drogon/HttpAppFramework.h>
#include <fmt/core.h>
#include <trantor/utils/Logger.h>

namespace sgrn::datastore::audit
{

drogon::Task<bool> AuditLogger::log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
    std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type, std::string t_target_id,
    std::string t_ip, Json::Value t_details, std::string t_status) {
    try {
        if (!tsp_db_client) {
            tsp_db_client = drogon::app().getDbClient();
        }
        if (!tsp_db_client) {
            co_return false;
        }

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string details_str = Json::writeString(wb, t_details);

        // Insert atomically via core.append_audit_log() under advisory lock
        co_await tsp_db_client->execSqlCoro(
            "SELECT out_id, out_seq, out_prev_hash, out_row_hash FROM core.append_audit_log("
            "$1::text, $2::text, $3::int, $4::text, $5::text, $6::text, $7::text, $8::inet, $9::jsonb, $10::text)",
            t_organisation, t_actor_type, t_actor_id, t_actor_name, t_action,
            t_target_type.empty() ? std::optional<std::string>{} : t_target_type,
            t_target_id.empty() ? std::optional<std::string>{} : t_target_id, t_ip.empty() ? "127.0.0.1" : t_ip, details_str, t_status);

        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "AuditLogger failed to append audit record '" << t_action << "': " << e.what();
        co_return false;
    }
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
            auto org_res = co_await tsp_db_client->execSqlCoro("SELECT DISTINCT organisation FROM core.audit_logs ORDER BY organisation");
            for (const auto& row : org_res) {
                orgs_to_verify.push_back(row["organisation"].as<std::string>());
            }
        }

        if (orgs_to_verify.empty()) {
            result.is_valid = true;
            result.failure_reason = "No audit log entries found";
            co_return result;
        }

        for (const auto& org : orgs_to_verify) {
            auto verify_res =
                co_await tsp_db_client->execSqlCoro("SELECT is_valid, total_rows, first_broken_seq, first_broken_id, failure_reason "
                                                    "FROM core.verify_audit_chain($1::text)",
                    org);

            if (verify_res.empty()) {
                result.is_valid = false;
                result.failure_reason = "core.verify_audit_chain returned no rows for org: " + org;
                co_return result;
            }

            const auto& row = verify_res[0];
            const bool valid = row["is_valid"].as<bool>();
            const int64_t rows = row["total_rows"].as<int64_t>();
            result.total_rows += rows;

            if (!valid) {
                result.is_valid = false;
                if (!row["first_broken_seq"].isNull()) {
                    result.first_broken_seq = row["first_broken_seq"].as<int64_t>();
                }
                if (!row["first_broken_id"].isNull()) {
                    result.first_broken_id = row["first_broken_id"].as<int64_t>();
                }
                result.failure_reason = "Org '" + org + "': " + row["failure_reason"].as<std::string>();
                co_return result;
            }

            // Verify checkpoints for this org
            auto chk_res =
                co_await tsp_db_client->execSqlCoro("SELECT id, last_seq, last_hash, signature, public_key "
                                                    "FROM core.audit_checkpoints WHERE organisation = $1::text ORDER BY last_seq ASC",
                    org);

            for (const auto& chk : chk_res) {
                if (chk["signature"].isNull() || chk["public_key"].isNull()) {
                    continue;
                }
                const std::string sig_hex = chk["signature"].as<std::string>();
                const std::string pub_hex = chk["public_key"].as<std::string>();
                const int64_t last_seq = chk["last_seq"].as<int64_t>();
                const std::string last_hash = chk["last_hash"].as<std::string>();

                const std::string checkpoint_payload = org + ":" + std::to_string(last_seq) + ":" + last_hash;
                if (!verifyEd25519(checkpoint_payload, sig_hex, pub_hex)) {
                    result.is_valid = false;
                    result.first_broken_seq = last_seq;
                    result.failure_reason =
                        "Checkpoint Ed25519 signature verification failed for checkpoint ID " + std::to_string(chk["id"].as<int64_t>());
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
        auto latest_res = co_await tsp_db_client->execSqlCoro(
            "SELECT seq, row_hash FROM core.audit_logs WHERE organisation = $1::text ORDER BY seq DESC LIMIT 1", t_organisation);

        if (latest_res.empty()) {
            result.error = "No audit log records exist for organisation: " + t_organisation;
            co_return result;
        }

        const int64_t last_seq = latest_res[0]["seq"].as<int64_t>();
        const std::string last_hash = latest_res[0]["row_hash"].as<std::string>();

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
            "INSERT INTO core.audit_checkpoints (organisation, last_seq, last_hash, sink_target, signature, public_key) "
            "VALUES ($1::text, $2::bigint, $3::text, $4::text, $5::text, $6::text) "
            "RETURNING id",
            t_organisation, last_seq, last_hash, t_sink_target, sig_hex, pub_hex);

        result.success = true;
        result.checkpoint_id = ins_res[0]["id"].as<int64_t>();
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
