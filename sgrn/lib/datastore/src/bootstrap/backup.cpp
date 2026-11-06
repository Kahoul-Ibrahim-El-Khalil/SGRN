// sgrn/lib/datastore/src/bootstrap/backup.cpp
//
// `--backup-db`, `--backup-garage` and `--verify-backup` — definitions for
// include/sgrn/datastore/bootstrap/backup.hpp. Everything not declared in
// that header (S3 session, DB context, manifest helpers) is file-local.

#include <sgrn/datastore/bootstrap/backup.hpp>

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/bootstrap/secrets.hpp> // resolveEnvPlaceholderKey
#include <sgrn/debug.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/hashing.hpp> // computeSha256File
#include <algorithm>
#include <array>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap::backup
{

// The sgrn::utils re-exports used below (utcStamp, formatBytes,
// jsonEscapeInto, machineName, ProgressMeter, Sha256Stream, TarWriter,
// ensureOwnerOnlyPerms, …) come from the header.

namespace
{

/// psql/pg_dump live next to each other: prefer SGRN_DEPLOYMENT_ENV/bin
/// (same precedence as executeSchemaViaPsql), otherwise fall back to PATH.
std::string deploymentBinPrefix() {
    const std::string deployment_env = sgrn::utils::env::get("SGRN_DEPLOYMENT_ENV", "");
    if (!deployment_env.empty() && deployment_env.find("$ENV{") == std::string::npos) {
        const fs::path bin = fs::path(deployment_env) / "bin";
        std::error_code ec;
        if (fs::exists(bin, ec)) {
            return (bin / "").string(); // trailing slash by convention
        }
    }
    return {};
}

int discardArtifact(const fs::path& t_tmp) {
    std::error_code ec;
    fs::remove(t_tmp, ec);
    return 1;
}

// ---------------------------------------------------------------------------
// shared DB context (counts + object catalog); read-only superuser peer auth
// ---------------------------------------------------------------------------

struct DbContext {
    std::string host;
    std::string port;
    std::string db;
    std::string superuser;
    std::string conn;
};

sgrn::Result<void, std::string> loadEnvFile(const fs::path& t_env_path) {
    return sgrn::utils::env::loadFile(t_env_path);
}

std::string envOrDefault(const char* t_key, const std::string& t_default) {
    return sgrn::utils::env::get(t_key, t_default);
}

DbContext connectDbContext(const std::string& t_base_dir) {
    namespace fs = std::filesystem;
    const fs::path env_path = fs::path(t_base_dir) / ".env";
    if (!fs::exists(env_path)) {
        SGRN_ERROR("DatastoreBackup", "Configuration file .env not found. Please run --generate-config first.");
        std::exit(EXIT_FAILURE);
    }
    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreBackup", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }
    resolveEnvPlaceholderKey(env_path, "SGRN_DEPLOYMENT_ENV");

    DbContext ctx;
    ctx.host = envOrDefault("POSTGRES_HOST", "127.0.0.1");
    ctx.port = envOrDefault("POSTGRES_PORT", "5432");
    ctx.db = envOrDefault("POSTGRES_DB", "sgrn");
    ctx.superuser = envOrDefault("POSTGRES_SUPERUSER", "postgres");
    ctx.conn = fmt::format("host={} port={} dbname={} user={}", ctx.host, ctx.port, ctx.db, ctx.superuser);
    if (const char* su_pass = ::getenv("POSTGRES_SUPERUSER_PASSWORD"); su_pass != nullptr) {
        ctx.conn += fmt::format(" password={}", su_pass);
    }
    return ctx;
}

long long scalarCount(drogon::orm::DbClient& t_client, const std::string& t_sql) {
    try {
        auto result = t_client.execSqlSync(t_sql);
        return result.empty() ? 0 : result[0][0].template as<long long>();
    } catch (const std::exception& e) {
        SGRN_WARN("DatastoreBackup", "Count query failed ({}): {}", t_sql.substr(0, 40), e.what());
        return -1;
    }
}

/// findLatestFile wrapper: newest archive of t_suffix inside t_dir.
fs::path findLatestArchive(const fs::path& t_dir, const std::string& t_suffix) {
    return sgrn::utils::filesystem::findLatestFile(t_dir, t_suffix);
}

// ---------------------------------------------------------------------------
// AWS helper: synchronous GetObject straight into a writer + digester.
// ---------------------------------------------------------------------------

struct S3FetchResult {
    bool ok = false;
    int http_status = 0;
    std::string sha256_hex;
    std::uint64_t bytes = 0;
    std::string error;
};

class S3Session {
public:
    S3Session() {
        Aws::SDKOptions options;
        options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Off;
        Aws::InitAPI(options);
        options_ = options;

        const std::string endpoint = sgrn::utils::env::get("GARAGE_S3_ENDPOINT", "");
        const std::string region = sgrn::utils::env::get("GARAGE_REGION", "us-east-1");
        const std::string access = sgrn::utils::env::get("GARAGE_ACCESS_KEY", "");
        const std::string secret = sgrn::utils::env::get("GARAGE_SECRET_KEY", "");
        if (access.empty() || secret.empty() || endpoint.empty() || endpoint.find("$ENV{") != std::string::npos) {
            ready_ = false;
            error_ = "GARAGE_S3_ENDPOINT / GARAGE_ACCESS_KEY / GARAGE_SECRET_KEY missing or unresolved in .env — run --init first";
            return;
        }

        Aws::S3::S3ClientConfiguration cfg;
        cfg.region = region;
        cfg.scheme = Aws::Http::Scheme::HTTP;
        if (endpoint.rfind("https://", 0) == 0) {
            cfg.scheme = Aws::Http::Scheme::HTTPS;
        }
        cfg.endpointOverride = Aws::String(endpoint.substr(endpoint.find("://") + 3));
        cfg.useVirtualAddressing = false; // garage is path-style
        client_ = std::make_shared<Aws::S3::S3Client>(Aws::Auth::AWSCredentials(access, secret), nullptr, cfg);
        ready_ = true;
    }
    ~S3Session() {
        client_.reset();
        Aws::ShutdownAPI(options_);
    }
    S3Session(const S3Session&) = delete;
    S3Session& operator=(const S3Session&) = delete;

    bool ready() const {
        return ready_;
    }
    const std::string& error() const {
        return error_;
    }

    /// GET (bucket,key) streaming into t_out + inline sha256.
    S3FetchResult get(const std::string& t_bucket, const std::string& t_key, std::ofstream& t_out) {
        S3FetchResult res;
        if (!ready()) {
            res.error = error_;
            return res;
        }
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(t_bucket);
        req.SetKey(t_key);
        auto outcome = client_->GetObject(req);
        if (!outcome.IsSuccess()) {
            res.http_status = static_cast<int>(outcome.GetError().GetResponseCode());
            res.error = outcome.GetError().GetMessage();
            return res;
        }
        res.http_status = 200;
        auto& body = outcome.GetResult().GetBody();
        Sha256Stream hasher;
        std::array<char, 256 * 1024> buf{};
        while (body.good()) {
            body.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = body.gcount();
            if (got <= 0) {
                break;
            }
            t_out.write(buf.data(), got);
            hasher.update(buf.data(), static_cast<size_t>(got));
            res.bytes += static_cast<std::uint64_t>(got);
            if (!t_out) {
                res.error = "backup tar write failed (disk full?)";
                return res;
            }
        }
        res.sha256_hex = hasher.hex();
        res.ok = true;
        return res;
    }

private:
    Aws::SDKOptions options_{};
    std::shared_ptr<Aws::S3::S3Client> client_;
    bool ready_ = false;
    std::string error_;
};

// --verify-backup : archive member walk against the PostgreSQL catalog.
int verifyTarArchiveAgainstDb(const std::string& t_base_dir, const fs::path& t_tar_path) {
    if (!fs::exists(t_tar_path)) {
        fmt::print(stderr, "error: archive '{}' does not exist\n", t_tar_path.string());
        return 1;
    }
    const DbContext ctx = connectDbContext(t_base_dir);
    trantor::EventLoopThread loop_thread;
    loop_thread.run();
    auto client = drogon::orm::DbClient::newPgClient(ctx.conn, 1, loop_thread.getLoop());

    struct CatalogEntry {
        std::string sha256;
        std::int64_t size = 0;
    };
    std::unordered_map<std::string, CatalogEntry> db_objects;
    std::uintmax_t db_total_bytes = 0;
    try {
        auto result = client->execSqlSync("SELECT bucket, key, sha256, size FROM storage.objects");
        for (const auto& row : result) {
            const std::string b = row["bucket"].template as<std::string>();
            const std::string k = row["key"].template as<std::string>();
            std::string s = row["sha256"].template as<std::string>();
            while (!s.empty() && s.back() == ' ') {
                s.pop_back();
            }
            const auto sz = row["size"].template as<std::int64_t>();
            if (sz > 0) {
                db_total_bytes += static_cast<std::uintmax_t>(sz);
            }
            db_objects[b + "/" + k] = {s, sz};
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "error: cannot read storage.objects from database: {}\n", e.what());
        return 1;
    }

    std::ifstream in(t_tar_path, std::ios::binary);
    if (!in) {
        fmt::print(stderr, "error: cannot open {}\n", t_tar_path.string());
        return 1;
    }

    fmt::print("Verifying archive '{}' against PostgreSQL ({}:{}/{} - {} catalog objects, ~{})\n", t_tar_path.filename().string(), ctx.host,
        ctx.port, ctx.db, db_objects.size(), formatBytes(db_total_bytes));

    std::array<char, 512> h{};
    std::string pending_long_name;
    size_t tar_count = 0, matched = 0, mismatched = 0, uncataloged = 0;
    std::uintmax_t tar_bytes = 0;
    std::unordered_set<std::string> seen_members;
    std::vector<std::string> mismatch_details;

    const std::uintmax_t archive_size = fs::file_size(t_tar_path);
    ProgressMeter meter(db_objects.size(), archive_size);

    while (in.read(h.data(), 512)) {
        if (h[0] == '\0') {
            break;
        }
        char size_buf[13] = {};
        std::memcpy(size_buf, h.data() + 124, 11);
        const std::uintmax_t fsize = std::strtoull(size_buf, nullptr, 8);
        const char typeflag = h[156];

        if (typeflag == 'L') {
            std::string lname;
            lname.resize(fsize);
            in.read(lname.data(), static_cast<std::streamsize>(fsize));
            while (!lname.empty() && lname.back() == '\0') {
                lname.pop_back();
            }
            pending_long_name = lname;
            const size_t pad = (512 - (fsize % 512)) % 512;
            if (pad > 0) {
                in.seekg(static_cast<std::streamoff>(pad), std::ios::cur);
            }
            continue;
        }

        if (typeflag == '0' || typeflag == '\0') {
            std::string member;
            if (!pending_long_name.empty()) {
                member = pending_long_name;
                pending_long_name.clear();
            } else {
                size_t name_len = 0;
                while (name_len < 100 && h[name_len] != '\0') {
                    ++name_len;
                }
                const std::string name(h.data(), name_len);

                size_t prefix_len = 0;
                while (prefix_len < 155 && h[329 + prefix_len] != '\0') {
                    ++prefix_len;
                }
                const std::string prefix(h.data() + 329, prefix_len);
                member = prefix.empty() ? name : prefix + "/" + name;
            }

            Sha256Stream hasher;
            std::uintmax_t remain = fsize;
            std::array<char, 64 * 1024> chunk{};
            while (remain > 0) {
                const size_t want = static_cast<size_t>(std::min<std::uintmax_t>(remain, chunk.size()));
                in.read(chunk.data(), static_cast<std::streamsize>(want));
                hasher.update(chunk.data(), want);
                remain -= want;
            }
            const size_t pad = (512 - (fsize % 512)) % 512;
            if (pad > 0) {
                in.seekg(static_cast<std::streamoff>(pad), std::ios::cur);
            }

            const std::string hex = hasher.hex();
            seen_members.insert(member);
            ++tar_count;
            tar_bytes += fsize;

            meter.update(tar_count, static_cast<std::uintmax_t>(in.tellg()));

            auto it = db_objects.find(member);
            if (it == db_objects.end()) {
                ++uncataloged;
            } else {
                if (it->second.sha256 == hex && it->second.size == static_cast<std::int64_t>(fsize)) {
                    ++matched;
                } else {
                    ++mismatched;
                    if (mismatch_details.size() < 10) {
                        mismatch_details.push_back(
                            fmt::format("{} (tar sha: {}..., db sha: {}...)", member, hex.substr(0, 10), it->second.sha256.substr(0, 10)));
                    }
                }
            }
        } else {
            const size_t pad = (512 - (fsize % 512)) % 512;
            in.seekg(static_cast<std::streamoff>(fsize + pad), std::ios::cur);
        }
    }
    meter.finish(tar_count, tar_bytes);

    std::vector<std::string> missing_in_archive;
    for (const auto& [db_path, _] : db_objects) {
        if (seen_members.find(db_path) == seen_members.end()) {
            if (missing_in_archive.size() < 10) {
                missing_in_archive.push_back(db_path);
            }
        }
    }
    const size_t missing_count = db_objects.size() > matched ? db_objects.size() - matched : 0;
    const bool is_ok = (missing_in_archive.empty() && mismatched == 0 && matched == db_objects.size());

    fmt::print("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (is_ok) {
        fmt::print("  ✓ ARCHIVE & DATABASE CONSISTENCY 100% VERIFIED\n");
        fmt::print("    • Database catalog objects  : {}\n", db_objects.size());
        fmt::print("    • Tar archive objects       : {} ({})\n", tar_count, formatBytes(tar_bytes));
        fmt::print("    • Verified SHA-256 matches  : {} / {} (100%)\n", matched, db_objects.size());
        fmt::print("    • Missing in archive        : 0\n");
        fmt::print("    • Corrupt / mismatched      : 0\n");
        if (uncataloged > 0) {
            fmt::print("    • Uncataloged extra in tar  : {}\n", uncataloged);
        }
    } else {
        fmt::print("  ✗ CONSISTENCY CHECK FAILED: archive does not match database!\n");
        fmt::print("    • Database catalog objects  : {}\n", db_objects.size());
        fmt::print("    • Tar archive objects       : {}\n", tar_count);
        fmt::print("    • Verified matches          : {}\n", matched);
        if (!missing_in_archive.empty()) {
            fmt::print("    • Missing in archive        : {} (sample below)\n", missing_count);
            for (const auto& m : missing_in_archive) {
                fmt::print(stderr, "        - {}\n", m);
            }
        }
        if (mismatched > 0) {
            fmt::print("    • SHA-256 mismatches        : {}\n", mismatched);
            for (const auto& m : mismatch_details) {
                fmt::print(stderr, "        - {}\n", m);
            }
        }
    }
    fmt::print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    return is_ok ? 0 : 1;
}

} // namespace

// ---------------------------------------------------------------------------
// atomic-artifact primitives
// ---------------------------------------------------------------------------

fs::path prepareBackupDir(const std::string& t_base_dir) {
    const fs::path backup_dir = fs::path(t_base_dir) / "backup";
    fs::create_directories(backup_dir);
    // Orphan .tmp files from an interrupted run are garbage; clean them so
    // nothing half-built can ever be mistaken for an artifact.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(backup_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (!ec && name.find(".tmp") != std::string::npos && entry.is_regular_file(ec)) {
            fs::remove(entry.path());
        }
    }
    return backup_dir;
}

bool publishArtifact(const fs::path& t_tmp, const fs::path& t_final) {
    std::error_code ec;
    fs::rename(t_tmp, t_final, ec);
    if (ec) {
        fmt::print(stderr, "error: could not promote {} -> {} ({})\n", t_tmp.string(), t_final.string(), ec.message());
        return false;
    }
    ensureOwnerOnlyPerms(t_final);
    return true;
}

// ---------------------------------------------------------------------------
// --backup-db : pg_dump -Fc -Z9, atomic, verified, manifest
// ---------------------------------------------------------------------------

int backupDatabase(const std::string& t_base_dir) {
    const DbContext ctx = connectDbContext(t_base_dir);
    const fs::path backup_dir = prepareBackupDir(t_base_dir);
    const std::string stamp = utcStamp("%Y%m%dT%H%M%SZ");

    // Catalog counts for the manifest — restore reconciles against these.
    trantor::EventLoopThread loop_thread;
    loop_thread.run();
    auto client = drogon::orm::DbClient::newPgClient(ctx.conn, 1, loop_thread.getLoop());
    const long long users = scalarCount(*client, "SELECT count(*) FROM core.users");
    const long long files = scalarCount(*client, "SELECT count(*) FROM storage.files");
    const long long objects = scalarCount(*client, "SELECT count(*) FROM storage.objects");
    long long objects_bytes = scalarCount(*client, "SELECT COALESCE(sum(size),0) FROM storage.objects");
    if (objects_bytes < 0) {
        objects_bytes = 0;
    }

    const std::string tmp_name = fmt::format(".sgrn-pg-{}.{}.tmp", stamp, ::getpid());
    const fs::path tmp = backup_dir / tmp_name;

    fmt::print("Backing up PostgreSQL {}:{}/{}\n", ctx.host, ctx.port, ctx.db);
    fmt::print("  catalog snapshot: {} users, {} files, {} objects (~{} bytes referenced)\n", users, files, objects, objects_bytes);
    fmt::print("  hint: consistent point-in-time requires the datastore stopped\n");

    std::string env_prefix;
    if (const char* su_pass = ::getenv("POSTGRES_SUPERUSER_PASSWORD"); su_pass != nullptr) {
        env_prefix = fmt::format("PGPASSWORD='{}' ", su_pass);
    }
    const std::string cmd = fmt::format("{}{}pg_dump -h {} -p {} -U {} -d {} -Fc -Z9 -f {}", env_prefix, deploymentBinPrefix(), ctx.host,
        ctx.port, ctx.superuser, ctx.db, tmp.string());
    const auto dump_start = std::chrono::steady_clock::now();
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        fmt::print(stderr, "error: pg_dump failed (exit {})\n", rc);
        return discardArtifact(tmp);
    }
    const auto dump_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - dump_start).count();
    fmt::print("  pg_dump finished in {:.2f}s\n", dump_sec);

    // Pg_restore sanity: the artifact must at least present a coherent TOC.
    const std::string verify_cmd = fmt::format("{}pg_restore --list {} > /dev/null 2>&1", deploymentBinPrefix(), tmp.string());
    if (std::system(verify_cmd.c_str()) != 0) {
        fmt::print(stderr, "error: dump did not produce a loadable pg_restore TOC; artifact discarded\n");
        return discardArtifact(tmp);
    }

    std::string arch = "n/a";
    if (auto digest = sgrn::utils::computeSha256File(tmp, sgrn::utils::HashEncoding::hex); digest.hasValue()) {
        arch = digest.value();
    }
    const std::string artifact = fmt::format("sgrn-bck-{}.pg", stamp);
    const fs::path final_path = backup_dir / artifact;
    if (!publishArtifact(tmp, final_path)) {
        return 1;
    }

    // Manifest: written after promotion so a manifest never describes an
    // artifact that is not yet visible.
    std::error_code size_ec;
    const std::uintmax_t bytes = fs::file_size(final_path, size_ec);
    std::string manifest = "{\n";
    auto field = [&manifest](const std::string& k, const std::string& v, bool last = false, bool quoted = true) {
        manifest += "  \"" + k + "\": ";
        if (quoted) {
            manifest += "\"";
            jsonEscapeInto(manifest, v);
            manifest += "\"";
        } else {
            manifest += v;
        }
        manifest += last ? "\n" : ",\n";
    };
    field("kind", "database");
    field("artifact", artifact);
    field("created_utc", utcStamp("%Y-%m-%dT%H:%M:%SZ"));
    field("source_host", machineName());
    field("postgres_host", ctx.host);
    field("postgres_port", ctx.port);
    field("postgres_db", ctx.db);
    field("pg_dump", "-Fc -Z9 (pg_restore --clean to restore)");
    field("bytes", std::to_string(bytes), false, false);
    field("sha256", arch);
    field("users", std::to_string(users), false, false);
    field("files", std::to_string(files), false, false);
    field("objects", std::to_string(objects), false, false);
    field("objects_bytes", std::to_string(objects_bytes), true, false);
    manifest += "}\n";

    const fs::path manifest_path = backup_dir / (artifact + ".manifest.json");
    {
        std::ofstream out(manifest_path, std::ios::trunc);
        out << manifest;
    }
    ensureOwnerOnlyPerms(manifest_path);

    fmt::print("  artifact: {}\n  sha256: {}\n  manifest: {}\n\n", final_path.string(), arch, manifest_path.string());
    SGRN_INFO("DatastoreBackup", "Database backup complete.");
    return 0;
}

// ---------------------------------------------------------------------------
// --backup-garage : catalog-driven object drain into a plain ustar tar
// ---------------------------------------------------------------------------

int backupGarage(const std::string& t_base_dir) {
    const DbContext ctx = connectDbContext(t_base_dir);
    const fs::path backup_dir = prepareBackupDir(t_base_dir);
    const std::string stamp = utcStamp("%Y%m%dT%H%M%SZ");

    // 1. The DB row for every object: (bucket, key, sha256, size = stored size)
    trantor::EventLoopThread loop_thread;
    loop_thread.run();
    auto client = drogon::orm::DbClient::newPgClient(ctx.conn, 1, loop_thread.getLoop());
    struct Row {
        std::string bucket;
        std::string key;
        std::string sha256;
        std::int64_t size = 0;
    };
    std::vector<Row> rows;
    try {
        auto result = client->execSqlSync("SELECT bucket, key, sha256, size FROM storage.objects ORDER BY bucket, key");
        rows.reserve(result.size());
        for (const auto& row : result) {
            Row r;
            r.bucket = row["bucket"].template as<std::string>();
            r.key = row["key"].template as<std::string>();
            r.sha256 = row["sha256"].template as<std::string>();
            r.size = row["size"].template as<std::int64_t>();
            // shar256 is bpchar: right-padded, trim trailing spaces defensively
            while (!r.sha256.empty() && r.sha256.back() == ' ') {
                r.sha256.pop_back();
            }
            rows.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "error: cannot read storage.objects catalog: {}\n", e.what());
        return 1;
    }

    std::uintmax_t total_bytes = 0;
    for (const auto& r : rows) {
        if (r.size > 0) {
            total_bytes += static_cast<std::uintmax_t>(r.size);
        }
    }
    fmt::print("Backing up Garage objects (catalog {}:{}: {} object(s), ~{} bytes)\n", ctx.host, ctx.port, rows.size(), total_bytes);
    fmt::print("  hint: consistent point-in-time requires the datastore stopped\n");

    // 2. S3 session straight from .env values.
    S3Session s3;
    if (!s3.ready()) {
        fmt::print(stderr, "error: {}\n", s3.error());
        return 1;
    }

    // 3. stream into a plain tar; per-object sha256 must match the catalog.
    const std::string tmp_name = fmt::format(".sgrn-garage-{}.{}.tmp", stamp, ::getpid());
    const fs::path tmp = backup_dir / tmp_name;
    TarWriter tar(tmp);
    if (!tar.ok()) {
        fmt::print(stderr, "error: cannot create {}\n", tmp.string());
        return 1;
    }
    Sha256Stream tar_digest;

    size_t fetched = 0, skipped_missing = 0, skipped_mismatch = 0;
    std::uintmax_t carried_bytes = 0;
    std::vector<std::string> missing_sample, mismatch_sample;
    ProgressMeter meter(rows.size(), total_bytes);
    size_t current_idx = 0;
    for (const auto& row : rows) {
        ++current_idx;
        if (row.sha256.size() != 64) {
            ++skipped_mismatch;
            if (mismatch_sample.size() < 5) {
                mismatch_sample.push_back(fmt::format("{}/{} (bad sha256 column)", row.bucket, row.key));
            }
            meter.update(current_idx, carried_bytes);
            continue;
        }
        // Per object: fetch to a small scratch file (bounded disk extra =
        // the largest single object), verify sha256 + size against the
        // catalog row, then append into the tar. A missing object (not in
        // Garage) or a sha/size mismatch is skipped, counted and disclosed
        // in the manifest — one bad row never destroys the rest of the
        // archive, and the manifest is the restore-side reconciliation
        // contract either way.
        const std::string member = row.bucket + "/" + row.key;
        const fs::path scratch = backup_dir / fmt::format(".sgrn-obj-{}.bin", ::getpid());
        std::ofstream sink(scratch, std::ios::binary | std::ios::trunc);
        const S3FetchResult res = s3.get(row.bucket, row.key, sink);
        sink.close();
        if (!res.ok) {
            ++skipped_missing;
            if (missing_sample.size() < 5) {
                missing_sample.push_back(
                    fmt::format("{}/{} ({})", row.bucket, row.key, res.error.empty() ? "unknown S3 error" : res.error));
            }
            std::error_code ec;
            fs::remove(scratch, ec);
            meter.update(current_idx, carried_bytes);
            continue;
        }
        // fetched sha256/size already computed during the stream; the
        // catalog row (bpchar sha256, possibly padded) is the expectation.
        std::string expected = row.sha256;
        while (!expected.empty() && expected.back() == ' ') {
            expected.pop_back();
        }
        if (expected.size() > 64) {
            expected = expected.substr(expected.size() - 64);
        }
        if (res.sha256_hex != expected || res.bytes != static_cast<std::uintmax_t>(row.size)) {
            ++skipped_mismatch;
            if (mismatch_sample.size() < 5) {
                mismatch_sample.push_back(fmt::format("{}/{} (got {} bytes, sha {}... vs {}...)", row.bucket, row.key, res.bytes,
                    res.sha256_hex.substr(0, 12), expected.substr(0, 12)));
            }
            std::error_code ec;
            fs::remove(scratch, ec);
            meter.update(current_idx, carried_bytes);
            continue;
        }
        const std::uintmax_t fetched_size = res.bytes;
        if (!tar.addFile(member, scratch, fetched_size, tar_digest)) {
            fmt::print(stderr, "error: tar append failed for {} — aborting\n", member);
            fs::remove(scratch);
            return discardArtifact(tmp);
        }
        fs::remove(scratch);
        ++fetched;
        carried_bytes += fetched_size;
        meter.update(current_idx, carried_bytes);
    }
    meter.finish(fetched, carried_bytes);
    if (!tar.finalize(tar_digest)) {
        fmt::print(stderr, "error: tar finalize failed\n");
        return discardArtifact(tmp);
    }

    std::string arch = "n/a";
    if (auto digest = sgrn::utils::computeSha256File(tmp, sgrn::utils::HashEncoding::hex); digest.hasValue()) {
        arch = digest.value();
    }
    const std::string artifact = fmt::format("sgrn-bck-{}.garage.tar", stamp);
    const fs::path final_path = backup_dir / artifact;
    if (!publishArtifact(tmp, final_path)) {
        return 1;
    }

    std::error_code size_ec;
    const std::uintmax_t bytes = fs::file_size(final_path, size_ec);
    std::string manifest = "{\n";
    auto field = [&manifest](const std::string& k, const std::string& v, bool last = false, bool quoted = true) {
        manifest += "  \"" + k + "\": ";
        if (quoted) {
            manifest += "\"";
            jsonEscapeInto(manifest, v);
            manifest += "\"";
        } else {
            manifest += v;
        }
        manifest += last ? "\n" : ",\n";
    };
    auto arrayField = [&manifest](const std::string& k, const std::vector<std::string>& t_items, bool last = false) {
        manifest += "  \"" + k + "\": [";
        for (size_t i = 0; i < t_items.size(); ++i) {
            manifest += i == 0 ? "\n" : ",\n";
            manifest += "    \"";
            jsonEscapeInto(manifest, t_items[i]);
            manifest += "\"";
        }
        manifest += t_items.empty() ? "]" : "\n  ]";
        manifest += last ? "\n" : ",\n";
    };
    field("kind", "garage");
    field("artifact", artifact);
    field("created_utc", utcStamp("%Y-%m-%dT%H:%M:%SZ"));
    field("source_host", machineName());
    field("catalog_host", ctx.host);
    field("catalog_port", ctx.port);
    field("catalog_db", ctx.db);
    field("tar", "plain ustar, NO compression (content is codec/object-level compressed)");
    field("objects_total", std::to_string(rows.size()), false, false);
    field("objects_carried", std::to_string(fetched), false, false);
    field("bytes", std::to_string(bytes), false, false);
    field("carried_object_bytes", std::to_string(carried_bytes), false, false);
    field("sha256", arch);
    field("missing_objects", std::to_string(skipped_missing), false, false);
    field("sha_mismatches", std::to_string(skipped_mismatch), true, false);
    if (!missing_sample.empty()) {
        arrayField("missing_sample", missing_sample);
    }
    if (!mismatch_sample.empty()) {
        arrayField("mismatch_sample", mismatch_sample, true);
    }
    manifest += "}\n";

    const fs::path manifest_path = backup_dir / (artifact + ".manifest.json");
    {
        std::ofstream out(manifest_path, std::ios::trunc);
        out << manifest;
    }
    ensureOwnerOnlyPerms(manifest_path);

    const bool is_consistent = (skipped_missing == 0 && skipped_mismatch == 0 && fetched == rows.size());

    fmt::print("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (is_consistent) {
        fmt::print("  ✓ CONSISTENCY VERIFIED: 100% of cataloged objects accounted for\n");
        fmt::print("    • Database catalog    : {} objects ({})\n", rows.size(), formatBytes(total_bytes));
        fmt::print("    • Verified in Garage  : {} / {} (SHA-256 match)\n", fetched, rows.size());
        fmt::print("    • Missing objects     : 0\n");
        fmt::print("    • Corrupt / mismatch  : 0\n");
    } else {
        fmt::print("  ✗ CONSISTENCY WARNING: catalog drift or corrupt objects detected!\n");
        fmt::print("    • Database catalog    : {} objects ({})\n", rows.size(), formatBytes(total_bytes));
        fmt::print("    • Verified & archived : {} objects ({})\n", fetched, formatBytes(carried_bytes));
        if (skipped_missing > 0) {
            fmt::print("    • Missing in Garage   : {} (listed in manifest)\n", skipped_missing);
            for (const auto& m : missing_sample) {
                fmt::print(stderr, "        - {}\n", m);
            }
        }
        if (skipped_mismatch > 0) {
            fmt::print("    • SHA-256 mismatches  : {} (listed in manifest)\n", skipped_mismatch);
            for (const auto& m : mismatch_sample) {
                fmt::print(stderr, "        - {}\n", m);
            }
        }
    }
    fmt::print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    fmt::print("  artifact: {}\n  sha256: {}\n  manifest: {}\n\n", final_path.string(), arch, manifest_path.string());
    SGRN_INFO("DatastoreBackup", "Garage backup complete.");
    return is_consistent ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --verify-backup : cross-verify backup archive(s) against PostgreSQL catalog
// ---------------------------------------------------------------------------

int verifyBackup(const std::string& t_base_dir, const std::string& t_target) {
    const fs::path backup_dir = fs::path(t_base_dir) / "backup";
    if (!t_target.empty()) {
        const fs::path p(t_target);
        if (p.extension() == ".tar") {
            return verifyTarArchiveAgainstDb(t_base_dir, p);
        }
        if (p.extension() == ".pg") {
            const std::string verify_cmd = fmt::format("{}pg_restore --list {} > /dev/null 2>&1", deploymentBinPrefix(), p.string());
            if (std::system(verify_cmd.c_str()) == 0) {
                fmt::print("✓ PostgreSQL dump '{}' verified (valid TOC)\n", p.filename().string());
                return 0;
            } else {
                fmt::print(stderr, "✗ PostgreSQL dump '{}' failed TOC verification\n", p.filename().string());
                return 1;
            }
        }
        fmt::print(stderr, "error: unknown backup artifact type for '{}'\n", t_target);
        return 1;
    }

    const fs::path latest_pg = findLatestArchive(backup_dir, ".pg");
    const fs::path latest_tar = findLatestArchive(backup_dir, ".garage.tar");

    if (latest_pg.empty() && latest_tar.empty()) {
        fmt::print(stderr, "error: no backup artifacts found in {}\n", backup_dir.string());
        return 1;
    }

    int rc = 0;
    if (!latest_pg.empty()) {
        const std::string verify_cmd = fmt::format("{}pg_restore --list {} > /dev/null 2>&1", deploymentBinPrefix(), latest_pg.string());
        if (std::system(verify_cmd.c_str()) == 0) {
            fmt::print("PostgreSQL dump '{}' verified (valid TOC)\n", latest_pg.filename().string());
        } else {
            fmt::print(stderr, " PostgreSQL dump '{}' failed TOC verification\n", latest_pg.filename().string());
            rc = 1;
        }
    }
    if (!latest_tar.empty()) {
        if (verifyTarArchiveAgainstDb(t_base_dir, latest_tar) != 0) {
            rc = 1;
        }
    }
    return rc;
}

} // namespace sgrn::datastore::bootstrap::backup
