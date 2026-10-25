#pragma once

// pg_fixture.hpp — a disposable PostgreSQL database for datastore C++ tests.
//
// Three modes, decided per construction:
//
//   1. external  — PGHOST is set: create a scratch database on that server
//                  using the PG* env credentials (PGPORT/PGUSER/PGPASSWORD),
//                  replay the SQL tree into it, drop it on destruction.
//   2. ephemeral — no PGHOST: locate initdb/postgres (SGRN_PG_BIN dir, else
//                  PATH), initdb a single-user cluster into a temp dir and
//                  start it on OS-assigned ports with trust auth. Everything
//                  is removed on destruction.
//   3. unavailable — neither applies (e.g. no PG binaries on PATH): the fixture
//                  reports `available() == false` + `skipReason()` and the
//                  test prints SKIP and exits 0 — the same convention the
//                  Python orchestrator (tests/run_tests.py) and crud_binds_test
//                  use for environment-dependent tests.
//
// Schema replay is a small psql-subset flattener over sgrn/lib/datastore/
// postgres/**: recursive `\i`/`\ir`, `${POSTGRES_*}` placeholder substitution,
// and a statement splitter that honours `$$`/`$tag$` dollar quoting, single
// quotes, and `--`/`/* */` comments. Top-level postgres/init.sql is NOT
// replayed (it drops+creates+`\c`s the database); the in-database files are
// applied in the same order init.sql does.

#include "test_util.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace sgrn::test
{

class PgFixture {
public:
    /// t_scratch_prefix names the scratch database in external mode; must be a
    /// bare SQL identifier-safe string (letters, digits, underscore).
    explicit PgFixture(const std::string& t_scratch_prefix = "sgrn_test") {
        if (const char* p_host = ::getenv("PGHOST"); p_host && *p_host)
            initExternal();
        else
            initEphemeral(t_scratch_prefix);

        if (m_available)
            replaySchema(); // schema errors surface through skipReason
    }

    ~PgFixture() {
        teardown();
    }

    PgFixture(const PgFixture&) = delete;
    PgFixture& operator=(const PgFixture&) = delete;

    // ── Surface ───────────────────────────────────────────────────────────────

    bool available() const {
        return m_available;
    }
    const std::string& skipReason() const {
        return m_skip_reason;
    }

    /// libpq connection string for the admin (schema owner) connection.
    const std::string& adminConnInfo() const {
        return m_admin_conninfo;
    }

    /// libpq connection string for the sgrn_datastore API role. Only valid
    /// when roleCredentialsKnown() — the ephemeral cluster sets the role
    /// password itself; an external server keeps whatever password it has, so
    /// the test must not assume it.
    const std::string& roleConnInfo() const {
        return m_role_conninfo;
    }
    bool roleCredentialsKnown() const {
        return m_role_password_known;
    }

    const std::string& dbName() const {
        return m_db_name;
    }

private:
    // ── Mode 1: external server via PG* envs ────────────────────────────────
    void initExternal() {
        m_external = true;

        const std::string host = ::getenv("PGHOST");
        const std::string port = (::getenv("PGPORT") && *::getenv("PGPORT")) ? ::getenv("PGPORT") : "5432";
        const char* p_user = (::getenv("PGUSER") && *::getenv("PGUSER")) ? ::getenv("PGUSER") : ::getlogin();
        const std::string user = (p_user && *p_user) ? p_user : "postgres";
        const char* password = ::getenv("PGPASSWORD");
        m_pg_password = password ? password : "";

        m_admin_conninfo = "host=" + host + " port=" + port + " user=" + user +
                           (m_pg_password.empty() ? "" : " password=" + m_pg_password) + " dbname=postgres";
        m_admin_conn = ::PQconnectdb(m_admin_conninfo.c_str());
        if (::PQstatus(m_admin_conn) != CONNECTION_OK) {
            m_skip_reason = std::string("cannot reach external PostgreSQL (PGHOST set): ") + ::PQerrorMessage(m_admin_conn);
            teardown();
            return;
        }

        m_db_name = "sgrn_scratch_" + std::to_string(::getpid());
        if (!exec("CREATE DATABASE " + m_db_name)) {
            m_skip_reason = "cannot create scratch database: " + m_last_error;
            teardown();
            return;
        }

        m_admin_conninfo = "host=" + host + " port=" + port + " user=" + user +
                           (m_pg_password.empty() ? "" : " password=" + m_pg_password) + " dbname=" + m_db_name;
        ::PQfinish(m_admin_conn);
        m_admin_conn = ::PQconnectdb(m_admin_conninfo.c_str());
        if (::PQstatus(m_admin_conn) != CONNECTION_OK) {
            m_skip_reason = std::string("cannot connect to scratch database: ") + ::PQerrorMessage(m_admin_conn);
            teardown();
            return;
        }
        m_available = true;
    }

    // ── Mode 2: ephemeral initdb/postgres cluster ────────────────────────────
    void initEphemeral(const std::string&) {
        // Locate binaries: SGRN_PG_BIN dir, else which(3) on PATH.
        const std::string bin_dir = ::getenv("SGRN_PG_BIN") ? ::getenv("SGRN_PG_BIN") : "";
        auto locate = [&](const char* t_name) -> std::string {
            if (!bin_dir.empty()) {
                const std::string p = bin_dir + "/" + t_name;
                if (sgrn::test::pathExists(p))
                    return p;
            }
            // which(3) is POSIX.1-2008 but glibc hides it behind feature
            // macros; scanning PATH instead keeps the header portable.
            const char* p_path = ::getenv("PATH");
            if (p_path) {
                const std::string path(p_path);
                std::size_t prev = 0;
                while (prev <= path.size()) {
                    const std::size_t sep = path.find(':', prev);
                    const std::string dir = path.substr(prev, sep == std::string::npos ? std::string::npos : sep - prev);
                    if (!dir.empty() && sgrn::test::pathExists(dir + "/" + t_name))
                        return dir + "/" + t_name;
                    if (sep == std::string::npos)
                        break;
                    prev = sep + 1;
                }
            }
            // Convenience fallback: this project's micromamba environment
            // ships the PG toolchain (ctest runs without it activated).
            const char* p_home = ::getenv("HOME");
            if (p_home) {
                const std::string p = std::string(p_home) + "/micromamba/envs/SGRN/bin/" + t_name;
                if (sgrn::test::pathExists(p))
                    return p;
            }
            return {};
        };

        const std::string initdb = locate("initdb");
        const std::string postgres = locate("postgres");
        if (initdb.empty() || postgres.empty()) {
            m_skip_reason = "no PostgreSQL installation found (set SGRN_PG_BIN or PGHOST, or put initdb+postgres on PATH)";
            return;
        }

        m_work_dir = sgrn::test::makeTempDir("sgrn-pg");
        m_pg_data = m_work_dir + "/data";
        m_db_port = sgrn::test::freePort();
        if (m_db_port <= 0) {
            m_skip_reason = "no free loopback port available for the ephemeral cluster";
            teardown();
            return;
        }

        std::string error;
        // Single named superuser, trust auth, disable locale dependencies so
        // schema replay is deterministic on any host.
        const int rc = sgrn::test::runProcess({initdb, "-D", m_pg_data, "-U", "sgrn_test", "--auth=trust", "--no-locale"}, {}, {}, error);
        if (rc != 0) {
            m_skip_reason = std::string("initdb failed (rc=") + std::to_string(rc) + "): " + error;
            teardown();
            return;
        }

        // Start without TCP listener (unix socket in the work dir) — no port
        // squatting beyond the socket filename, no firewall interactions.
        std::string sock_dir = m_work_dir + "/sock";
        if (::mkdir(sock_dir.c_str(), 0700) != 0) {
            m_skip_reason = std::string("mkdir(sockdir): ") + std::strerror(errno);
            teardown();
            return;
        }
        SpawnResult res = sgrn::test::spawnProcess({postgres, "-D", m_pg_data, "-p", std::to_string(m_db_port), "-k", sock_dir, "-c",
                                                       "fsync=off", "-c", "synchronous_commit=off", "-c", "full_page_writes=off"},
            {}, {});
        if (res.pid < 0) {
            m_skip_reason = std::string("cannot start postgres: ") + res.error;
            teardown();
            return;
        }
        m_server = Process(res);

        // Wait for readiness (~10s): initdb of an isolated cluster is ~1-2s.
        const std::string base = "host=" + sock_dir + " port=" + std::to_string(m_db_port) + " user=sgrn_test dbname=postgres";
        PGconn* probe = nullptr;
        for (int i = 0; i < 100 && !probe; ++i) {
            if (!m_server.live()) {
                m_skip_reason = "postgres exited during startup (check the fixture log)";
                teardown();
                return;
            }
            PGconn* attempt = ::PQconnectdb(base.c_str());
            if (::PQstatus(attempt) == CONNECTION_OK)
                probe = attempt; // keep it: confirms readiness, closed below
            else
                ::PQfinish(attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!probe) {
            m_skip_reason = "ephemeral postgres never became ready";
            teardown();
            return;
        }
        ::PQfinish(probe);

        m_admin_conn = ::PQconnectdb(base.c_str());
        if (::PQstatus(m_admin_conn) != CONNECTION_OK) {
            m_skip_reason = std::string("cannot connect to ephemeral postgres: ") + ::PQerrorMessage(m_admin_conn);
            teardown();
            return;
        }

        m_db_name = "sgrn_scratch_" + std::to_string(::getpid());
        if (!exec("CREATE DATABASE " + m_db_name)) {
            m_skip_reason = "cannot create scratch database: " + m_last_error;
            teardown();
            return;
        }

        // Reopen admin connection on the scratch database.
        m_admin_conninfo = "host=" + sock_dir + " port=" + std::to_string(m_db_port) + " user=sgrn_test dbname=" + m_db_name;
        ::PQfinish(m_admin_conn);
        m_admin_conn = ::PQconnectdb(m_admin_conninfo.c_str());
        if (::PQstatus(m_admin_conn) != CONNECTION_OK) {
            m_skip_reason = std::string("cannot connect to scratch database: ") + ::PQerrorMessage(m_admin_conn);
            teardown();
            return;
        }

        // The API role is created by roles.sql with a password we define
        // here (substituted into ${POSTGRES_PASSWORD}) — trust auth ignores
        // it server-side, but roleConnInfo() still carries it so the test
        // exercises the real login path.
        m_pg_password = "sgrn-role-test-password";
        m_role_password_known = true;
        m_role_conninfo = "host=" + sock_dir + " port=" + std::to_string(m_db_port) + " user=sgrn_datastore password=" + m_pg_password +
                          " dbname=" + m_db_name;

        m_subst.POSTGRES_DB = m_db_name;
        m_subst.POSTGRES_USER = "sgrn_test";
        m_subst.POSTGRES_PASSWORD = m_pg_password;

        m_available = true;
    }

    // ── Statement splitting + replay ─────────────────────────────────────────

    // Structural note: init.sql (top level) must NOT be replayed — it does
    // `DROP DATABASE`/`CREATE DATABASE`/`\c` which this fixture implements in
    // C++ instead. Everything inside the database comes from this list, in the
    // order top-level init.sql includes them.
    static constexpr const char* kReplayFiles[] = {"schemas/init.sql", "roles.sql", "views/init.sql", "functions/init.sql", "seeding.sql",
        "seed_test_automated_service.sql", "seed_dev_service.sql"};

    struct Subst {
        std::string POSTGRES_DB;
        std::string POSTGRES_USER;
        std::string POSTGRES_PASSWORD;
    };

    void replaySchema() {
        namespace fs = std::filesystem;
        fs::path root = fs::path(sgrn::test::repoRoot()) / "sgrn" / "lib" / "datastore" / "postgres";
        // Top-level init.sql also creates pgcrypto (crypt/gen_salt/
        // gen_random_bytes are used by roles/seeding/auth functions) — the
        // database-level statements it contains are implemented by the
        // fixture itself.
        if (!exec("CREATE EXTENSION IF NOT EXISTS pgcrypto")) {
            m_skip_reason = "cannot create pgcrypto: " + m_last_error;
            m_available = false;
            return;
        }
        // The loading order follows the include lines of top-level init.sql;
        // functions/init.sql and views/init.sql are thin index files that are
        // replayed wholesale (their own \ir lines recurse with dir-relative
        // resolution).
        for (const char* rel : kReplayFiles) {
            fs::path file = root / rel;
            if (!sgrn::test::pathExists(file.string())) {
                m_skip_reason = std::string("missing postgres SQL file: ") + rel;
                m_available = false;
                return;
            }
            std::string dir = file.parent_path().string();
            if (!replayFile(file.string(), dir)) {
                m_skip_reason = std::string("schema replay failed on ") + rel + ": " + m_last_error;
                m_available = false;
                return;
            }
        }
    }

    /// Process one SQL file: handle `\i`/`\ir` (recursive, dir-relative) and
    /// skip other psql meta commands (\set, \echo, \c…), running everything
    /// between them through the statement splitter.
    bool replayFile(const std::string& t_path, const std::string& t_dir) {
        if (std::find(m_in_flight.begin(), m_in_flight.end(), t_path) != m_in_flight.end()) {
            m_last_error = "include cycle at " + t_path;
            return false;
        }
        m_in_flight.push_back(t_path);

        namespace fs = std::filesystem;

        const std::string text = sgrn::test::readFile(t_path);
        bool ok = true;
        std::size_t pos = 0;
        std::string buffer; // SQL accumulated between meta commands

        auto flush = [&]() {
            if (!buffer.empty()) {
                ok = ok && runStatements(buffer, t_path);
                buffer.clear();
            }
        };

        while (ok && pos < text.size()) {
            // Lines are cheap here: replay files are <100KB.
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string::npos)
                eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;

            // Trim right, keep leading whitespace checks simple
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            const std::string trimmed = [&] {
                std::size_t s = line.find_first_not_of(" \t");
                return s == std::string::npos ? std::string() : line.substr(s);
            }();

            if (!trimmed.empty() && trimmed.front() == '\\') {
                // psql meta command
                if (trimmed.rfind("\\ir", 0) == 0 || trimmed.rfind("\\i ", 0) == 0 || trimmed == "\\i") {
                    flush();
                    // tokenise: \ir <path>
                    std::size_t s = 3;
                    while (s < trimmed.size() && (trimmed[s] == ' ' || trimmed[s] == '\t'))
                        ++s;
                    std::string inc = trimmed.substr(s);
                    // strip trailing comment on the include line
                    if (const std::size_t cm = inc.find("--"); cm != std::string::npos)
                        inc = inc.substr(0, cm);
                    while (!inc.empty() && (inc.back() == ' ' || inc.back() == '\t'))
                        inc.pop_back();
                    const std::string inc_path = (fs::path(inc).is_absolute()) ? inc : (fs::path(t_dir) / inc).lexically_normal().string();
                    if (!sgrn::test::pathExists(inc_path)) {
                        m_last_error = t_path + ": included file not found: " + inc + " (" + inc_path + ")";
                        ok = false;
                    } else if (!replayFile(inc_path, fs::path(inc_path).parent_path().string())) {
                        m_last_error = t_path + ": include failed: " + m_last_error;
                        ok = false;
                    }
                }
                // \set / \echo / \c / \q etc. — none apply inside the fixture
                // (the scratch database already exists; \c would re-point to a
                // different db and must not be replayed).
                continue;
            }

            buffer += line;
            buffer += '\n';
        }
        flush();

        m_in_flight.pop_back();
        return ok;
    }

    /// Execute statements one-by-one; `;` at top level ends a statement.
    /// Dollar-quoted strings ($$…$$, $tag$…$tag$), single-quoted strings,
    /// double-quoted identifiers and both comment styles are transparent.
    bool runStatements(const std::string& t_sql, const std::string& t_source) {
        std::string stmt;
        std::size_t i = 0;
        const std::size_t n = t_sql.size();

        while (i < n) {
            const char c = t_sql[i];

            if (c == '-' && i + 1 < n && t_sql[i + 1] == '-') {
                while (i < n && t_sql[i] != '\n')
                    ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && t_sql[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(t_sql[i] == '*' && t_sql[i + 1] == '/'))
                    ++i;
                i = std::min(i + 2, n);
                continue;
            }
            if (c == '\'') {
                // ''-escapes, E'\n' style keep the quote-pair rule simple: end at the next unescaped '
                stmt += c;
                ++i;
                while (i < n) {
                    stmt += t_sql[i];
                    if (t_sql[i] == '\'') {
                        if (i + 1 < n && t_sql[i + 1] == '\'') {
                            stmt += '\'';
                            i += 2;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (c == '"') {
                stmt += c;
                ++i;
                while (i < n) {
                    stmt += t_sql[i];
                    if (t_sql[i] == '"') {
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (c == '$') {
                // dollar-quote start? $$ or $tag$ (tag = [A-Za-z_][A-Za-z0-9_]*)
                bool is_dollar_quote = false;
                std::size_t j = i + 1;
                if (j < n && t_sql[j] == '$')
                    is_dollar_quote = true, ++j;
                else {
                    const std::size_t start = j;
                    while (j < n && (std::isalnum(static_cast<unsigned char>(t_sql[j])) || t_sql[j] == '_'))
                        ++j;
                    if (j > start && j < n && t_sql[j] == '$')
                        is_dollar_quote = true, ++j;
                }
                if (is_dollar_quote) {
                    const std::string tag = t_sql.substr(i, j - i); // includes both $
                    const std::size_t end = t_sql.find(tag, j);
                    if (end == std::string::npos) {
                        m_last_error = t_source + ": unterminated dollar quote " + tag;
                        return false;
                    }
                    stmt += t_sql.substr(i, end + tag.size() - i);
                    i = end + tag.size();
                    continue;
                }
                // $1-style param — ordinary character
                stmt += c;
                ++i;
                continue;
            }
            if (c == ';') {
                ++i;
                const std::string clean = [&] {
                    std::size_t s = stmt.find_first_not_of(" \t\r\n");
                    if (s == std::string::npos)
                        return std::string();
                    std::size_t e = stmt.find_last_not_of(" \t\r\n");
                    return stmt.substr(s, e - s + 1);
                }();
                stmt.clear();
                if (clean.empty())
                    continue;
                if (!exec(clean)) {
                    m_last_error = t_source + " — " + m_last_error;
                    return false;
                }
                continue;
            }
            stmt += c;
            ++i;
        }

        // trailing statement without `;`
        std::size_t s = stmt.find_first_not_of(" \t\r\n");
        if (s == std::string::npos)
            return true;
        const std::string trimmed = stmt.substr(s);
        if (!exec(trimmed)) {
            m_last_error = t_source + " — " + m_last_error;
            return false;
        }
        return true;
    }

    // ── execution + teardown ──────────────────────────────────────────────────

    /// Substitute ${POSTGRES_*} placeholders (roles.sql wires the role password).
    bool exec(std::string t_sql) {
        for (std::pair<const char*, const std::string*> sub : {std::pair{"${POSTGRES_DB}", &m_subst.POSTGRES_DB},
                 std::pair{"${POSTGRES_USER}", &m_subst.POSTGRES_USER}, std::pair{"${POSTGRES_PASSWORD}", &m_subst.POSTGRES_PASSWORD}}) {
            std::size_t at;
            while ((at = t_sql.find(sub.first)) != std::string::npos)
                t_sql.replace(at, std::strlen(sub.first), *sub.second);
        }

        // DDL/DML for a scratch test database: notices (RAISE NOTICE … from
        // seeding) are noise, silence them via a no-op handler.
        ::PQsetNoticeProcessor(m_admin_conn, [](void*, const char*) noexcept {}, nullptr);
        PGresult* r = ::PQexec(m_admin_conn, t_sql.c_str());
        const ExecStatusType st = ::PQresultStatus(r);
        const bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK || st == PGRES_SINGLE_TUPLE || st == PGRES_EMPTY_QUERY);
        if (!ok)
            m_last_error = std::string(::PQresultErrorMessage(r));
        ::PQclear(r);
        return ok;
    }

    void teardown() {
        m_available = false;

        if (m_admin_conn) {
            if (m_external && !m_db_name.empty())
                ::PQexec(m_admin_conn, ("DROP DATABASE IF EXISTS " + m_db_name + " WITH (FORCE)").c_str());
            ::PQfinish(m_admin_conn);
            m_admin_conn = nullptr;
        }
        m_server.stop();
        if (!m_work_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(m_work_dir, ec);
            m_work_dir.clear();
        }
    }

    // ── state ────────────────────────────────────────────────────────────────
    bool m_external = false;
    std::string m_pg_password; // admin/role password in external mode

    std::string m_db_name;
    std::string m_admin_conninfo;
    std::string m_role_conninfo;
    bool m_role_password_known = false;

    std::string m_work_dir;
    std::string m_pg_data;
    int m_db_port = 0;
    Process m_server;

    PGconn* m_admin_conn = nullptr;
    Subst m_subst;
    std::vector<std::string> m_in_flight;

    std::string m_last_error;
    bool m_available = false;
    std::string m_skip_reason;
};

} // namespace sgrn::test
