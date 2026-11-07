// sgrn/lib/datastore/tests/verify-test.cpp
//
// Acceptance unit tests for Prompt 1:
// Verifies that the verify command catches each planted misconfiguration:
//   1. pg_hba containing 'trust'
//   2. pg_hba containing '0.0.0.0/0'
//   3. .env permissions looser than 0600 (e.g. 0644, 0755, 0666)
//   4. TLS cert is default self-signed without --allow-insecure
//   5. TLS cert is default self-signed with --allow-insecure (passes)
//   6. Clean configurations pass verification

#include <fmt/core.h>
#include <sgrn/datastore/bootstrap/verify.hpp>
#include <sgrn/utils/process.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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

namespace fs = std::filesystem;
using sgrn::datastore::bootstrap::verifyEnvPermissions;
using sgrn::datastore::bootstrap::verifyPgHbaSecurity;
using sgrn::datastore::bootstrap::VerifyReport;
using sgrn::datastore::bootstrap::verifyTlsCertificate;

void testEnvPermissions(const fs::path& t_sandbox) {
    fs::path env_path = t_sandbox / ".env";

    // 1. Planted misconfiguration: .env with 0644
    {
        std::ofstream out(env_path);
        out << "SGRN_USER=test\n";
    }
    fs::permissions(env_path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace);
    VerifyReport report1;
    bool ok1 = verifyEnvPermissions(env_path, report1);
    CHECK(!ok1);
    CHECK(report1.failed > 0);

    // 2. Planted misconfiguration: .env with 0777
    fs::permissions(env_path, fs::perms::all, fs::perm_options::replace);
    VerifyReport report2;
    bool ok2 = verifyEnvPermissions(env_path, report2);
    CHECK(!ok2);
    CHECK(report2.failed > 0);

    // 3. Compliant: .env with 0600
    fs::permissions(env_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    VerifyReport report3;
    bool ok3 = verifyEnvPermissions(env_path, report3);
    CHECK(ok3);
    CHECK(report3.failed == 0);
    CHECK(report3.passed == 1);
}

void testPgHbaSecurity(const fs::path& t_sandbox) {
    fs::path pghba_path = t_sandbox / "pg_hba.conf";

    // 1. Planted misconfiguration: 'trust'
    {
        std::ofstream out(pghba_path);
        out << "# pg_hba with trust\n";
        out << "local all all trust\n";
    }
    VerifyReport report1;
    bool ok1 = verifyPgHbaSecurity(pghba_path, report1);
    CHECK(!ok1);
    CHECK(report1.failed > 0);

    // 2. Planted misconfiguration: '0.0.0.0/0'
    {
        std::ofstream out(pghba_path);
        out << "# pg_hba with all-interfaces network rule\n";
        out << "host all all 0.0.0.0/0 scram-sha-256\n";
    }
    VerifyReport report2;
    bool ok2 = verifyPgHbaSecurity(pghba_path, report2);
    CHECK(!ok2);
    CHECK(report2.failed > 0);

    // 3. Planted misconfiguration: both trust and 0.0.0.0/0
    {
        std::ofstream out(pghba_path);
        out << "local all postgres peer\n";
        out << "host all all 0.0.0.0/0 trust\n";
    }
    VerifyReport report3;
    bool ok3 = verifyPgHbaSecurity(pghba_path, report3);
    CHECK(!ok3);
    CHECK(report3.failed >= 2);

    // 4. Compliant pg_hba.conf
    {
        std::ofstream out(pghba_path);
        out << "# Hardened configuration\n";
        out << "local all postgres peer\n";
        out << "local all sgrn_datastore scram-sha-256\n";
        out << "host all sgrn_datastore 127.0.0.1/32 scram-sha-256\n";
        out << "host all sgrn_datastore ::1/128 scram-sha-256\n";
    }
    VerifyReport report4;
    bool ok4 = verifyPgHbaSecurity(pghba_path, report4);
    CHECK(ok4);
    CHECK(report4.failed == 0);
    CHECK(report4.passed == 1);
}

void testTlsCertificate(const fs::path& t_sandbox) {
    fs::path cert_dir = t_sandbox / "certs";
    fs::create_directories(cert_dir);
    fs::path self_cert = cert_dir / "self.crt";
    fs::path self_key = cert_dir / "self.key";

    // Generate a self-signed certificate (subject == issuer == /CN=localhost)
    std::string gen_cmd =
        fmt::format("openssl req -x509 -nodes -days 30 -newkey rsa:2048 -keyout {} -out {} -subj \"/CN=localhost\" 2>/dev/null",
            self_key.string(), self_cert.string());
    int rc = sgrn::utils::process::execute(gen_cmd);
    CHECK(rc == 0);

    // 1. Planted misconfiguration: default self-signed cert WITHOUT --allow-insecure
    {
        VerifyReport report1;
        bool ok1 = verifyTlsCertificate(self_cert, false, report1);
        CHECK(!ok1);
        CHECK(report1.failed > 0);
    }

    // 2. Allowed when --allow-insecure IS passed
    {
        VerifyReport report2;
        bool ok2 = verifyTlsCertificate(self_cert, true, report2);
        CHECK(ok2);
        CHECK(report2.failed == 0);
        CHECK(report2.passed == 1);
    }

    // 3. CA-signed cert (issuer != subject) passes WITHOUT --allow-insecure
    fs::path ca_key = cert_dir / "ca.key";
    fs::path ca_cert = cert_dir / "ca.crt";
    fs::path srv_key = cert_dir / "ca_issued.key";
    fs::path srv_csr = cert_dir / "ca_issued.csr";
    fs::path srv_cert = cert_dir / "ca_issued.crt";

    // CA
    std::string ca_cmd =
        fmt::format("openssl req -x509 -nodes -days 30 -newkey rsa:2048 -keyout {} -out {} -subj \"/CN=RootCA\" 2>/dev/null",
            ca_key.string(), ca_cert.string());
    sgrn::utils::process::execute(ca_cmd);

    // Issued cert
    std::string csr_cmd = fmt::format("openssl req -nodes -newkey rsa:2048 -keyout {} -out {} -subj \"/CN=datastore.local\" 2>/dev/null",
        srv_key.string(), srv_csr.string());
    sgrn::utils::process::execute(csr_cmd);

    std::string sign_cmd = fmt::format("openssl x509 -req -in {} -CA {} -CAkey {} -CAcreateserial -out {} -days 30 2>/dev/null",
        srv_csr.string(), ca_cert.string(), ca_key.string(), srv_cert.string());
    sgrn::utils::process::execute(sign_cmd);

    if (fs::exists(srv_cert)) {
        VerifyReport report3;
        bool ok3 = verifyTlsCertificate(srv_cert, false, report3);
        CHECK(ok3);
        CHECK(report3.failed == 0);
        CHECK(report3.passed == 1);
    }
}

} // namespace

int main() {
    std::printf("Running verify_test...\n");
    fs::path sandbox = fs::temp_directory_path() / fmt::format("sgrn_verify_test_{:d}", ::getpid());
    fs::create_directories(sandbox);

    testEnvPermissions(sandbox);
    testPgHbaSecurity(sandbox);
    testTlsCertificate(sandbox);

    std::error_code ec;
    fs::remove_all(sandbox, ec);

    if (g_failures == 0) {
        std::printf("verify_test: ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "verify_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
