#pragma once
/**
 * @file  sgrn/datastore/bootstrap/verify.hpp
 * @brief Install health and drift verification for the SGRN datastore.
 *
 * Implements --verify-install: checks an already-deployed instance against
 * its staged templates and runtime prerequisites, printing PASS/FAIL lines.
 *
 * Designed against real outage fingerprints observed in production:
 *   - live unit files drifted from staged templates (env update)
 *   - staged nginx.conf carried unsubstituted ${...} placeholders, missed
 *     `daemon off;`, or had an empty sites-enabled (zero server blocks,
 *     nginx runs "active" yet listens on nothing)
 *   - datastore binary, TLS certs or nginx buffer dirs missing under the
 *     -p prefix (starts die with "[emerg] mkdir() ... failed")
 *   - garage not resolvable on the DEPLOYED unit's PATH (datastore then
 *     crash-loops on its dependency start gate)
 *
 * Live systemd/port checks are skipped outside systemd environments.
 * Definitions live in src/bootstrap/verify.cpp.
 */

#include <sgrn/debug.hpp>
#include <string>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Pass/fail ledger
// ---------------------------------------------------------------------------

/// Per-check pass/fail ledger for --verify-install; each entry logs one terse line.
struct VerifyReport {
    int passed = 0;
    int failed = 0;

    void pass(const std::string& t_what) {
        ++passed;
        SGRN_INFO("DatastoreInit", "  [ ok ] {}", t_what);
    }
    void fail(const std::string& t_what) {
        ++failed;
        SGRN_ERROR("DatastoreInit", "  [FAIL] {}", t_what);
    }
};

// ---------------------------------------------------------------------------
// verifyInstall orchestrator
// ---------------------------------------------------------------------------

int verifyInstall(const std::string& t_base_dir);

} // namespace sgrn::datastore::bootstrap
