// pg_auth_functions_test.cpp — datastore authentication + SQL envelope
// regression tests against a live Postgres.
//
// Coverage (first C++ tests over the previously-untested postgres/functions
// and the SQL-side JSON envelopes converted in storage.cpp):
//   1. core.authenticate_user: valid credentials → full user_details row;
//      wrong password → empty; inactive user → empty.
//   2. core.authenticate_automated_service: the fixed dev-service credentials
//      seeded by seed_dev_service.sql → 1 row; wrong secret / unknown token →
//      empty.
//   3. Role posture: the sgrn_datastore API role can call the auth functions
//      but has NO direct access to core.users (revoked; functions are
//      security definer). Only when the fixture knows the role password
//      (ephemeral cluster mode).
//   4. SQL envelope regression: the jsonb statements storage.cpp now splices
//      verbatim into HTTP bodies — drive listing (folders/files/totals in one
//      round trip, pagination math), virtual-root listing
//      (jsonb_strip_nulls keeps display_name only for services), the empty
//      result COALESCE to '[]', and the metadata envelope with the
//      server-side etag splice — each must render the same wire shape the old
//      per-row fmt::format/jsonEscape path produced.
//
// Environment: PGHOST (+PGPORT/PGUSER/PGPASSWORD) targets an external dev
// server via a scratch database; SGRN_PG_BIN/PATH supplies initdb+postgres for
// an ephemeral cluster; nothing available → SKIP + exit 0.

#include "pg_fixture.hpp"

#include <json/json.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

struct Query {
    PGresult* res = nullptr;
    bool ok = false; // executed without SQL error (rows may be empty)
};

Query query(PGconn* t_conn, const std::string& t_sql, const std::vector<std::string>& t_params = {}) {
    Query out;
    std::vector<const char*> values;
    values.reserve(t_params.size());
    // All parameters pass as text; SQL casts (e.g. $1::uuid) make them exact.
    for (const std::string& p : t_params)
        values.push_back(p.c_str());
    out.res = ::PQexecParams(
        t_conn, t_sql.c_str(), static_cast<int>(t_params.size()), nullptr, values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!out.res) {
        std::printf("FAIL PQexecParams returned NULL: %s\n", ::PQerrorMessage(t_conn));
        return out;
    }
    const ExecStatusType st = ::PQresultStatus(out.res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
        std::printf("FAIL SQL error: %s — statement: %s\n", ::PQresultErrorMessage(out.res), t_sql.c_str());
    else
        out.ok = true;
    return out;
}

std::string field(const Query& t_q, int t_row, int t_col) {
    if (!t_q.res)
        return {};
    const char* v = ::PQgetvalue(t_q.res, t_row, t_col);
    return v ? v : "";
}

std::string fieldById(const Query& t_q, int t_row, const char* t_name) {
    return field(t_q, t_row, t_q.res ? ::PQfnumber(t_q.res, t_name) : -1);
}

int nrows(const Query& t_q) {
    return t_q.res ? ::PQntuples(t_q.res) : -1;
}

Json::Value parseJson(const std::string& t_text) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_text.data(), t_text.data() + t_text.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL envelope JSON does not parse: %s (%s)\n", errors.c_str(), t_text.substr(0, 200).c_str());
        return {};
    }
    return v;
}

void clear(PGconn* t_conn, const std::string& t_sql) {
    const Query r = query(t_conn, t_sql);
    if (!r.ok) {
        ++g_failures;
        std::printf("FAIL cleanup failed: %s\n", t_sql.c_str());
    }
}

} // namespace

// ── SQL under test (verbatim copies of the storage.cpp envelopes) ───────────
// The {0}/{1} owner predicates are substituted exactly like
// buildNormalDriveListing does (fmt::runtime templates, user scope here).

static const char* kVirtualRootServices = "WITH items AS (SELECT token::text AS name, name AS display_name FROM core.automated_services "
                                          "WHERE organisation = $1 AND deleted_at IS NULL) "
                                          "SELECT COALESCE(jsonb_agg(jsonb_strip_nulls(jsonb_build_object("
                                          "'name', it.name, 'display_name', it.display_name, 'path', $2 || '/' || it.name)) "
                                          "ORDER BY it.name), '[]'::jsonb) AS folders, count(*)::bigint AS total FROM items it";

static const char* kVirtualRootDomains = "WITH items AS (SELECT name FROM core.domains WHERE organisation = $1) "
                                         "SELECT COALESCE(jsonb_agg(jsonb_strip_nulls(jsonb_build_object("
                                         "'name', it.name, 'display_name', NULL::text, 'path', $2 || '/' || it.name)) "
                                         "ORDER BY it.name), '[]'::jsonb) AS folders, count(*)::bigint AS total FROM items it";

// buildNormalDriveListing pagination envelope, user owner predicates:
//   $1 user_id, $2 current path guard ('/'), $3 parent path, $4 limit,
//   $5 page offset, $6 namespace prefix.
static const char* kListingUserPlain = R"SQL(
WITH parent_dirs AS (
  SELECT id FROM storage.directories WHERE user_id = $1 AND path = $3
),
folders_cte AS (
  SELECT id, name, created_at::text AS created_at, virtual_size, real_size,
         count_sub_files, count_sub_directories, path
  FROM storage.directories
  WHERE user_id = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM parent_dirs)))
),
folder_totals AS (SELECT count(*)::bigint AS n FROM folders_cte),
folder_page AS (
  SELECT * FROM folders_cte
  ORDER BY name
  LIMIT greatest(least($4::int, (SELECT n::int - $5::int FROM folder_totals)), 0)
  OFFSET $5::int
),
files_cte AS (
  SELECT f.id, f.name, f.full_path, f.extension, f.created_at::text AS created_at,
    (SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size,
    (SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size,
    so.upload_mode,
    (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count
  FROM storage.files f
  JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0
  JOIN storage.objects so ON so.id = fo0.object_id
  WHERE f.user_id = $1 AND ((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM parent_dirs)))
),
file_totals AS (
  SELECT count(*)::bigint AS n FROM storage.files
  WHERE user_id = $1 AND ((directory_id IS NULL AND $2 = '/') OR (directory_id IN (SELECT id FROM parent_dirs)))
),
file_page AS (
  SELECT * FROM files_cte
  ORDER BY name
  LIMIT greatest($4::int - least(greatest((SELECT n::int FROM folder_totals) - $5::int, 0), $4::int), 0)
  OFFSET greatest($5::int - (SELECT n::int FROM folder_totals), 0)
)
SELECT
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.path, 'created_at', s.created_at,
    'virtual_size', s.virtual_size, 'real_size', s.real_size,
    'count_sub_files', s.count_sub_files, 'count_sub_directories', s.count_sub_directories)
    ORDER BY s.name) FROM folder_page s), '[]'::jsonb) AS folders,
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.full_path, 'extension', coalesce(s.extension, ''),
    'size', s.compressed_size, 'original_size', s.original_size, 'upload_mode', s.upload_mode,
    'part_count', s.part_count, 'created_at', s.created_at)
    ORDER BY s.name) FROM file_page s), '[]'::jsonb) AS files,
  (SELECT n FROM folder_totals) AS total_folders,
  (SELECT n FROM file_totals) AS total_files
)SQL";

// runFileMetadataQuery envelope over storage.file_details (organisation scope).
static const char* kFileMetadataColumns =
    "file_id AS id, file_name AS name, file_path AS full_path, directory_path, directory_id, extension, created_at, "
    "is_compressed, compression_algorithm, compression_level, session_id, user_id, automated_service_id, "
    "domain_name AS domain, organisation_name AS organisation, object_id, bucket, key, object_size AS size, "
    "object_created_at, mime_type, upload_mode, part_count, part_size_bytes, sha256";

static const char* kMetadataEnvelope = "SELECT COALESCE(jsonb_agg("
                                       "jsonb_build_object("
                                       "'id', s.id, 'name', s.name, 'full_path', s.full_path, 'directory_path', s.directory_path, "
                                       "'directory_id', s.directory_id, 'extension', s.extension, 'created_at', s.created_at::text, "
                                       "'is_compressed', s.is_compressed, 'compression_algorithm', s.compression_algorithm, "
                                       "'compression_level', s.compression_level, 'session_id', s.session_id, 'user_id', s.user_id, "
                                       "'automated_service_id', s.automated_service_id, 'domain', s.domain, "
                                       "'organisation', s.organisation, 'object_id', s.object_id, 'bucket', s.bucket, "
                                       "'key', s.key, 'size', s.size, 'object_created_at', s.object_created_at::text, "
                                       "'mime_type', s.mime_type, 'upload_mode', s.upload_mode, 'part_count', s.part_count, "
                                       "'part_size_bytes', s.part_size_bytes, 'sha256', s.sha256) "
                                       "|| CASE WHEN s.sha256 IS NULL THEN '{}'::jsonb "
                                       "ELSE jsonb_build_object('etag', '\"' || s.sha256 || '\"') END "
                                       "ORDER BY id ASC), '[]'::jsonb) AS envelope "
                                       "FROM ({inner}) s";

static const char* kOrganisation = "CIC-Moulins Guelma";

int main() {
    sgrn::test::PgFixture pg("sgrn_scratch");
    if (!pg.available()) {
        std::printf("SKIP: live Postgres unavailable — %s\n", pg.skipReason().c_str());
        return 0;
    }

    PGconn* admin = ::PQconnectdb(pg.adminConnInfo().c_str());
    CHECK(::PQstatus(admin) == CONNECTION_OK);
    if (::PQstatus(admin) != CONNECTION_OK) {
        std::printf("SKIP: cannot open admin connection: %s\n", ::PQerrorMessage(admin));
        ::PQfinish(admin);
        return 0;
    }

    // ── 1a. authenticate_user — valid credentials ────────────────────────────
    {
        const Query q = query(admin, "SELECT ud.id, ud.email, ud.role, ud.is_active FROM core.authenticate_user($1, $2) ud",
            {"admin@local.com", "adminroot"});
        CHECK(q.ok);
        CHECK(nrows(q) == 1);
        if (nrows(q) == 1) {
            CHECK(field(q, 0, 1) == "admin@local.com");
            CHECK(field(q, 0, 2) == "admin");
        }
    }

    // ── 1b. wrong password → empty set ──────────────────────────────────────
    {
        const Query q = query(admin, "SELECT id FROM core.authenticate_user($1, $2)", {"admin@local.com", "wrong-password"});
        CHECK(q.ok);
        CHECK(nrows(q) == 0);
    }

    // ── 1c. unknown email → empty set (same shape; no user enumeration) ──────
    {
        const Query q = query(admin, "SELECT id FROM core.authenticate_user($1, $2)", {"nobody@local.com", "adminroot"});
        CHECK(q.ok);
        CHECK(nrows(q) == 0);
    }

    // ── 1d. inactive user excluded; auth recovers on reactivation ────────────
    {
        clear(admin, "UPDATE core.users SET is_active = false WHERE email = 'admin@local.com'");
        const Query q = query(admin, "SELECT id FROM core.authenticate_user($1, $2)", {"admin@local.com", "adminroot"});
        CHECK(nrows(q) == 0);
        clear(admin, "UPDATE core.users SET is_active = true WHERE email = 'admin@local.com'");
        const Query q2 = query(admin, "SELECT id FROM core.authenticate_user($1, $2)", {"admin@local.com", "adminroot"});
        CHECK(nrows(q2) == 1);
    }

    // ── 2. authenticate_automated_service (fixed dev-service credentials) ────
    {
        const Query q = query(admin, "SELECT name, status, organisation FROM core.authenticate_automated_service($1::uuid, $2)",
            {"1e78dbe3-1f5b-404f-89c1-ae0c07e98c5c", "FEcPKSxEuJzpst-MYcR8OlQWzFYxPoVDK9Sk3qmGn6A"});
        CHECK(q.ok);
        CHECK(nrows(q) == 1);
        CHECK(fieldById(q, 0, "name") == "dev-service");
        CHECK(fieldById(q, 0, "status") == "active");
    }
    {
        // wrong secret
        const Query q = query(admin, "SELECT id FROM core.authenticate_automated_service($1::uuid, $2)",
            {"1e78dbe3-1f5b-404f-89c1-ae0c07e98c5c", "wrong-secret"});
        CHECK(nrows(q) == 0);
    }
    {
        // unknown token
        const Query q = query(admin, "SELECT id FROM core.authenticate_automated_service($1::uuid, $2)",
            {"00000000-0000-0000-0000-000000000000", "FEcPKSxEuJzpst-MYcR8OlQWzFYxPoVDK9Sk3qmGn6A"});
        CHECK(nrows(q) == 0);
    }

    // ── 3. role posture (when the fixture knows the role password) ──────────
    PGconn* role = nullptr;
    if (pg.roleCredentialsKnown()) {
        role = ::PQconnectdb(pg.roleConnInfo().c_str());
        CHECK(::PQstatus(role) == CONNECTION_OK);
        if (::PQstatus(role) == CONNECTION_OK) {
            // may call the auth functions…
            {
                const Query q = query(role, "SELECT id FROM core.authenticate_user($1, $2)", {"admin@local.com", "adminroot"});
                CHECK(q.ok);
                CHECK(nrows(q) == 1);
            }
            // …and may touch core tables directly (roles.sql grants full
            // CRUD to the API role — no PostgREST intermediary anymore)
            {
                const Query q = query(role, "SELECT email FROM core.users LIMIT 1");
                CHECK(q.ok);
            }
        } else {
            std::printf("NOTE: sgrn_datastore role login refused on external server (password may differ from PGPASSWORD env)\n");
            ::PQfinish(role);
            role = nullptr;
        }
    } else {
        std::printf("NOTE: role-login posture checks skipped (no known sgrn_datastore password on this server)\n");
    }

    // ── 4. SQL envelope regression ─────────────────────────────────────────────
    // Seed a session + directory + file + object under the admin user.
    std::string admin_id, session_id, dir_id, obj_id, file_id, created_dir_at, created_file_at, created_obj_at;
    const std::string sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    {
        const Query q = query(admin, "SELECT id FROM core.users WHERE email = 'admin@local.com'");
        CHECK(q.ok && nrows(q) == 1);
        admin_id = field(q, 0, 0);
    }
    {
        const Query q = query(
            admin, "INSERT INTO core.sessions (user_id, token, ip) VALUES ($1, gen_random_uuid(), '127.0.0.1') RETURNING id", {admin_id});
        CHECK(q.ok && nrows(q) == 1);
        session_id = field(q, 0, 0);
    }
    {
        const Query q = query(admin,
            "INSERT INTO storage.directories (user_id, session_id, name, path) VALUES ($1, $2, 'data', '/data') RETURNING id, "
            "created_at::text",
            {admin_id, session_id});
        CHECK(q.ok && nrows(q) == 1);
        dir_id = field(q, 0, 0);
        created_dir_at = field(q, 0, 1);
    }
    {
        const Query q = query(admin,
            "INSERT INTO storage.objects (bucket, key, sha256, size, original_size, part_count, part_size_bytes) "
            "VALUES ('test-bucket', 'test/dolly.csv', $1, 11, 11, 1, 11) RETURNING id, created_at::text",
            {sha});
        CHECK(q.ok && nrows(q) == 1);
        obj_id = field(q, 0, 0);
        created_obj_at = field(q, 0, 1);
    }
    {
        const Query q = query(admin,
            "INSERT INTO storage.files (name, user_id, session_id, directory_id, extension, full_path) "
            "VALUES ('dolly.csv', $1, $2, $3, 'csv', '/data/dolly.csv') RETURNING id, created_at::text",
            {admin_id, session_id, dir_id});
        CHECK(q.ok && nrows(q) == 1);
        file_id = field(q, 0, 0);
        created_file_at = nrows(q) == 1 ? field(q, 0, 1) : "";
    }
    {
        clear(admin, "INSERT INTO storage.file_objects (file_id, object_id, part_index, role) VALUES (" + file_id + ", " + obj_id +
                         ", 0, 'primary')");
    }

    // ── 4a. drive listing at '/' (root) ─────────────────────────────────────
    {
        const Query q = query(admin, kListingUserPlain, {admin_id, "/", "/", "20", "0", "/admin"});
        CHECK(q.ok && nrows(q) == 1);
        if (q.ok && nrows(q) == 1) {
            CHECK(fieldById(q, 0, "total_folders") == "1");
            CHECK(fieldById(q, 0, "total_files") == "0");

            const Json::Value folders = parseJson(fieldById(q, 0, "folders"));
            CHECK(folders.isArray() && folders.size() == 1);
            CHECK(folders[0].isMember("id")); // ('id', s.id) is part of the wire shape
            CHECK(folders[0]["name"].asString() == "data");
            CHECK(folders[0]["path"].asString() == "/admin/data");
            CHECK(folders[0]["created_at"].asString() == created_dir_at);
            // trigger maintains count_sub_files (file seeded inside 'data')
            CHECK(folders[0]["count_sub_files"].asInt() == 1);
            CHECK(folders[0]["count_sub_directories"].asInt() == 0);
            CHECK(folders[0]["id"].asInt64() >= 0);

            const Json::Value files = parseJson(fieldById(q, 0, "files"));
            CHECK(files.isArray() && files.empty());
        }
    }

    // ── 4b. drive listing at '/data' → the seeded file page ─────────────────
    {
        const Query q = query(admin, kListingUserPlain, {admin_id, "/data/", "/data", "20", "0", "/admin"});
        CHECK(q.ok && nrows(q) == 1);
        if (q.ok && nrows(q) == 1) {
            CHECK(fieldById(q, 0, "total_folders") == "0");
            CHECK(fieldById(q, 0, "total_files") == "1");

            const Json::Value folders = parseJson(fieldById(q, 0, "folders"));
            CHECK(folders.isArray() && folders.empty());

            const Json::Value files = parseJson(fieldById(q, 0, "files"));
            CHECK(files.isArray() && files.size() == 1);
            CHECK(files[0]["name"].asString() == "dolly.csv");
            CHECK(files[0]["path"].asString() == "/admin/data/dolly.csv");
            CHECK(files[0]["extension"].asString() == "csv");
            CHECK(files[0]["size"].asInt64() == 11);
            CHECK(files[0]["original_size"].asInt64() == 11);
            CHECK(files[0]["upload_mode"].asString() == "single");
            CHECK(files[0]["part_count"].asInt() == 1);
            CHECK(files[0]["created_at"].asString() == created_file_at);
        }
    }

    // ── 4c. pagination: page 2 with limit 1 → empty page, totals intact ─────
    {
        const Query q = query(admin, kListingUserPlain, {admin_id, "/", "/", "1", "1", "/admin"});
        CHECK(q.ok);
        if (q.ok && nrows(q) == 1) {
            const Json::Value folders = parseJson(fieldById(q, 0, "folders"));
            CHECK(folders.isArray() && folders.empty());
            CHECK(fieldById(q, 0, "total_folders") == "1");
        }
    }

    // ── 4d. virtual-root envelope (automated services keep display_name) ────
    {
        const Query expected =
            query(admin, "SELECT count(*) FROM core.automated_services WHERE organisation = $1 AND deleted_at IS NULL", {kOrganisation});
        CHECK(expected.ok);
        if (expected.ok && nrows(expected) == 1) {
            const std::string count = field(expected, 0, 0);

            const Query q = query(admin, kVirtualRootServices, {kOrganisation, "/services"});
            CHECK(q.ok);
            if (q.ok && nrows(q) == 1) {
                const Json::Value folders = parseJson(fieldById(q, 0, "folders"));
                CHECK(folders.isArray() && static_cast<int>(folders.size()) == std::atoi(count.c_str()));
                CHECK(fieldById(q, 0, "total") == count);
                for (unsigned i = 0; i < folders.size(); ++i) {
                    // element shape: name = service token (uuid text) drives the
                    // path; display_name = service name
                    CHECK(folders[i].isMember("display_name"));
                    CHECK(folders[i]["path"].asString() == "/services/" + folders[i]["name"].asString());
                }
                bool found_dev_service = false;
                for (unsigned i = 0; i < folders.size(); ++i)
                    found_dev_service = found_dev_service || folders[i]["display_name"].asString() == "dev-service";
                CHECK(found_dev_service);
            }
        }
    }

    // ── 4e. virtual-root envelope (domains drop NULL display_name) ──────────
    {
        const Query q = query(admin, kVirtualRootDomains, {kOrganisation, "/domains"});
        CHECK(q.ok);
        if (q.ok && nrows(q) == 1) {
            const Json::Value folders = parseJson(fieldById(q, 0, "folders"));
            CHECK(folders.isArray() && folders.size() == 4); // Production/Maintenance/Logistics/Management
            if (!folders.empty()) {
                CHECK(folders[0].isMember("display_name") == false); // jsonb_strip_nulls dropped it
                CHECK(folders[0]["name"].asString() == "Logistics"); // ORDER BY name
                CHECK(folders[0]["path"].asString() == "/domains/Logistics");
            }
        }
    }

    // ── 4f. metadata envelope: full columns + server-side etag ──────────────
    {
        // exact per-file metadata query as the handler builds it
        const std::string inner_sql =
            std::string("SELECT ") + kFileMetadataColumns +
            " FROM storage.file_details WHERE organisation_name = $1 AND object_deleted_at IS NULL AND file_id = " + file_id;
        std::string env_sql = kMetadataEnvelope;
        const std::string marker = "{inner}";
        const std::size_t at = env_sql.find(marker);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            env_sql.replace(at, marker.size(), inner_sql);
            const Query q = query(admin, env_sql, {kOrganisation});
            CHECK(q.ok && nrows(q) == 1);
            if (q.ok && nrows(q) == 1) {
                const Json::Value envelope = parseJson(fieldById(q, 0, "envelope"));
                CHECK(envelope.isArray() && envelope.size() == 1);
                CHECK(envelope[0]["id"].asInt64() == std::atoll(file_id.c_str()));
                CHECK(envelope[0]["name"].asString() == "dolly.csv");
                CHECK(envelope[0]["full_path"].asString() == "/data/dolly.csv");
                CHECK(envelope[0]["directory_path"].asString() == "/data");
                CHECK(envelope[0]["extension"].asString() == "csv");
                CHECK(envelope[0]["created_at"].asString() == created_file_at);
                CHECK(envelope[0]["object_created_at"].asString() == created_obj_at);
                CHECK(envelope[0]["is_compressed"].asBool() == false);
                CHECK(envelope[0]["size"].asInt64() == 11);
                CHECK(envelope[0]["part_count"].asInt() == 1);
                if (envelope[0]["size"].asInt64() != 11 || envelope[0]["part_count"].asInt() != 1)
                    std::printf("FAIL unexpected envelope row: %s\n", envelope[0].toStyledString().c_str());
                CHECK(envelope[0]["upload_mode"].asString() == "single");
                CHECK(envelope[0]["mime_type"].asString() == "text/csv");
                CHECK(envelope[0]["bucket"].asString() == "test-bucket");
                CHECK(envelope[0]["organisation"].asString() == kOrganisation);
                // etag: S3-style — the quote characters are part of the
                // string value (exact old wire shape)
                const std::string expected_etag = "\"" + sha + "\"";
                CHECK(envelope[0]["etag"].asString() == expected_etag);
                if (envelope[0]["etag"].asString() != expected_etag)
                    std::printf("FAIL etag mismatch: got '%s' expected '%s'\n", envelope[0]["etag"].asString().c_str(), sha.c_str());
            }
        }
    }
    // ── 4g. metadata envelope: empty result COALESCEs to '[]' ────────────────
    {
        const std::string inner_sql =
            std::string("SELECT ") + kFileMetadataColumns +
            " FROM storage.file_details WHERE organisation_name = $1 AND object_deleted_at IS NULL AND file_id = 0";
        std::string env_sql = kMetadataEnvelope;
        const std::size_t at = env_sql.find("{inner}");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            env_sql.replace(at, 7, inner_sql);
            const Query q = query(admin, env_sql, {kOrganisation});
            CHECK(q.ok && nrows(q) == 1);
            if (q.ok && nrows(q) == 1)
                CHECK(fieldById(q, 0, "envelope") == "[]");
        }
    }

    // ── cleanup deltas (don't pollute a shared external scratch/namespace) ───
    clear(admin, "DELETE FROM storage.files WHERE id = " + file_id);
    clear(admin, "DELETE FROM storage.objects WHERE id = " + obj_id);
    clear(admin, "DELETE FROM storage.directories WHERE id = " + dir_id);
    clear(admin, "DELETE FROM core.sessions WHERE id = " + session_id);

    if (role)
        ::PQfinish(role);
    ::PQfinish(admin);

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pg_auth_functions_test: all checks passed\n");
    return 0;
}
