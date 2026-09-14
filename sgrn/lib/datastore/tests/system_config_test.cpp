// System-config helper unit tests (pure Json logic — no Drogon, no I/O).
//
// Covers the admin dashboard's safety contract: secrets are redacted and
// never writable, unknown keys are rejected fail-closed, the S3 multipart
// band is enforced, the 5MB part floor clamps with a warning, and valid
// updates merge without disturbing anything else (including "${...}"
// env placeholders, which must round-trip as opaque strings).

#include <sgrn/datastore/utils/system_config.hpp>

#include <json/json.h>

#include <cstdio>
#include <memory>
#include <string>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::datastore::sysconfig::applyAdminUpdate;
using sgrn::datastore::sysconfig::ApplyReport;
using sgrn::datastore::sysconfig::isKnownPath;
using sgrn::datastore::sysconfig::isSecretPath;
using sgrn::datastore::sysconfig::sanitizeForAdmin;
using sgrn::datastore::sysconfig::systemConfigSchema;
using sgrn::datastore::sysconfig::validateFormatExtension;
using sgrn::datastore::sysconfig::validateMimeType;

Json::Value parseJson(const std::string& t_raw) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    reader->parse(t_raw.data(), t_raw.data() + t_raw.size(), &v, &errors);
    return v;
}

Json::Value fileRoot() {
    return parseJson(R"({
        "custom_config": {
            "jwt_secret": "super-secret",
            "s3": {
                "default_bucket": "sgrn-uploads",
                "max_file_size_mb": 2048,
                "chunking_threshold_mb": 64,
                "chunk_part_size_mb": 12,
                "compression_level": 10
            },
            "rate_limiting": {"enabled": true, "storage_endpoint_limit": 30}
        },
        "db_clients": [{"passwd": "db-pass", "host": "127.0.0.1"}],
        "plugins": [{"config": {"secret_key": "garage-secret"}}]
    })");
}

} // namespace

int main() {
    // --- schema basics ------------------------------------------------------
    CHECK(!systemConfigSchema().empty());
    CHECK(isKnownPath("custom_config.s3.chunking_threshold_mb"));
    CHECK(!isKnownPath("custom_config.s3.nope"));
    CHECK(!isKnownPath("listeners"));

    // --- secrets ------------------------------------------------------------
    CHECK(isSecretPath("custom_config.jwt_secret"));
    CHECK(isSecretPath("db_clients.passwd"));
    CHECK(isSecretPath("plugins.config.secret_key"));
    CHECK(!isSecretPath("custom_config.s3.max_file_size_mb"));

    {
        const Json::Value clean = sanitizeForAdmin(fileRoot());
        CHECK(clean["custom_config"]["jwt_secret"].asString() == "***");
        CHECK(clean["db_clients"][0]["passwd"].asString() == "***");
        CHECK(clean["db_clients"][0]["host"].asString() == "127.0.0.1");
        CHECK(clean["plugins"][0]["config"]["secret_key"].asString() == "***");
        CHECK(clean["custom_config"]["s3"]["max_file_size_mb"].asUInt64() == 2048);
    }

    // --- unknown / secret keys rejected, root untouched ----------------------
    {
        Json::Value root = fileRoot();
        const Json::Value before = root;
        ApplyReport report;
        const std::string err = applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"nope": 1}}})"), report);
        CHECK(!err.empty());
        CHECK(root == before);
    }
    {
        Json::Value root = fileRoot();
        const Json::Value before = root;
        ApplyReport report;
        const std::string err = applyAdminUpdate(root, parseJson(R"({"custom_config": {"jwt_secret": "x"}})"), report);
        CHECK(!err.empty());
        CHECK(root == before);
    }

    // --- happy path: merge preserves everything else -------------------------
    {
        Json::Value root = parseJson(R"({
            "custom_config": {
                "s3": {"max_file_size_mb": 2048, "chunking_threshold_mb": 64, "chunk_part_size_mb": 12},
                "rate_limiting": {"general_endpoint_limit": 100}
            },
            "db_clients": [{"passwd": "${POSTGRES_PASSWORD}"}]
        })");
        ApplyReport report;
        const std::string err = applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"chunking_threshold_mb": 128}}})"), report);
        CHECK(err.empty());
        CHECK(root["custom_config"]["s3"]["chunking_threshold_mb"].asUInt64() == 128);
        CHECK(root["custom_config"]["s3"]["max_file_size_mb"].asUInt64() == 2048);
        CHECK(root["custom_config"]["rate_limiting"]["general_endpoint_limit"].asUInt() == 100);
        CHECK(root["db_clients"][0]["passwd"].asString() == "${POSTGRES_PASSWORD}");
        CHECK(report.hot.size() == 1 && report.hot[0] == "custom_config.s3.chunking_threshold_mb");
        CHECK(report.restart.empty());
    }

    // --- restart split --------------------------------------------------------
    {
        Json::Value root = fileRoot();
        ApplyReport report;
        const std::string err = applyAdminUpdate(
            root, parseJson(R"({"app": {"threads_num": 8}, "custom_config": {"s3": {"max_file_size_mb": 1024}}})"), report);
        CHECK(err.empty());
        CHECK(report.restart.size() == 1 && report.restart[0] == "app.threads_num");
        CHECK(report.hot.size() == 1 && report.hot[0] == "custom_config.s3.max_file_size_mb");
    }

    // --- 5MB part floor is enforced (strict via API, clamped on file load) ----
    {
        Json::Value root = fileRoot();
        const Json::Value before = root;
        ApplyReport report;
        // API path rejects below-minimum with a clear 400 reason...
        const std::string err = applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"chunk_part_size_mb": 2}}})"), report);
        CHECK(!err.empty());
        CHECK(root == before);
        // ...while a valid floor value applies cleanly.
        ApplyReport report2;
        CHECK(applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"chunk_part_size_mb": 5}}})"), report2).empty());
        CHECK(root["custom_config"]["s3"]["chunk_part_size_mb"].asUInt64() == 5);
    }

    // --- multipart band enforced ------------------------------------------------
    {
        Json::Value root = fileRoot();
        const Json::Value before = root;
        ApplyReport report;
        const std::string err = applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"chunking_threshold_mb": 2048}}})"), report);
        CHECK(!err.empty());
        CHECK(root == before);
    }

    // --- low debug thresholds accepted (single-part MPU is legal S3) ------------
    {
        Json::Value root = fileRoot();
        ApplyReport report;
        CHECK(applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"chunking_threshold_mb": 1}}})"), report).empty());
        CHECK(root["custom_config"]["s3"]["chunking_threshold_mb"].asUInt64() == 1);
        CHECK(report.hot.size() == 1);
    }

    // --- type + enum validation ---------------------------------------------------
    {
        Json::Value root = fileRoot();
        ApplyReport report;
        CHECK(!applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"max_file_size_mb": "big"}}})"), report).empty());
        CHECK(!applyAdminUpdate(root, parseJson(R"({"app": {"log_level": "VERBOSE"}})"), report).empty());
        CHECK(applyAdminUpdate(root, parseJson(R"({"app": {"log_level": "WARN"}})"), report).empty());
        // String arrays replace wholesale and validate element-wise.
        CHECK(applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"allowed_extensions": ["*", "pdf"]}}})"), report).empty());
        CHECK(!applyAdminUpdate(root, parseJson(R"({"custom_config": {"s3": {"allowed_extensions": ["pdf", 7]}}})"), report).empty());
    }

    // --- formats registry validation --------------------------------------------
    CHECK(validateFormatExtension("parquet").empty());
    CHECK(validateFormatExtension("zst").empty());
    CHECK(!validateFormatExtension("").empty());
    CHECK(!validateFormatExtension("PARQUET").empty());
    CHECK(!validateFormatExtension("has space").empty());
    CHECK(!validateFormatExtension("waytoolongextension").empty());
    CHECK(validateMimeType("application/vnd.apache.parquet").empty());
    CHECK(validateMimeType("text/csv").empty());
    CHECK(!validateMimeType("").empty());
    CHECK(!validateMimeType("not-a-mime").empty());
    CHECK(!validateMimeType("/json").empty());
    CHECK(!validateMimeType("text/ plain").empty());

    if (g_failures == 0) {
        std::printf("system_config_test: all checks passed\n");
    } else {
        std::printf("system_config_test: %d FAILURES\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
