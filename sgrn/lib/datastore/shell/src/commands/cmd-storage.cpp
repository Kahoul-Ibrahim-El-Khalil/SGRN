// cmd-storage.cpp — storage statistics, constraints and admin maintenance.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

sgrn::Result<void> DatastoreShell::cmdStats() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->storageStatsJson();
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

sgrn::Result<void> DatastoreShell::cmdInfo() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->storageConstraintsJson();
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

namespace
{

// --flag VALUE option parser shared by the storage-admin commands.
std::string shellFlag(const std::vector<std::string>& t_args, const std::string& t_flag, const std::string& t_dflt = "") {
    for (size_t i = 0; i + 1 < t_args.size(); ++i) {
        if (t_args[i] == t_flag)
            return t_args[i + 1];
    }
    return t_dflt;
}

bool shellHasFlag(const std::vector<std::string>& t_args, const std::string& t_flag) {
    return std::find(t_args.begin(), t_args.end(), t_flag) != t_args.end();
}

uint32_t shellUint(const std::vector<std::string>& t_args, const std::string& t_flag, uint32_t t_dflt) {
    const std::string raw = shellFlag(t_args, t_flag);
    if (raw.empty())
        return t_dflt;
    try {
        return static_cast<uint32_t>(std::stoul(raw));
    } catch (const std::exception&) {
        return t_dflt;
    }
}

} // namespace

sgrn::Result<void> DatastoreShell::cmdStorageOverview(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->tryStorageAdminOverview(shellUint(t_args, "--max-pages", 10));
    RETURN_IF(r.hasError(), r.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& b : r.value()) {
        rows.push_back({b.name_, std::to_string(b.garage_objects_) + (b.garage_truncated_ ? "+" : ""), std::to_string(b.garage_bytes_),
            std::to_string(b.db_objects_), std::to_string(b.db_bytes_), std::to_string(b.db_files_)});
    }
    printTable({"BUCKET", "GARAGE OBJS", "GARAGE BYTES", "DB OBJS", "DB BYTES", "FILES"}, std::move(rows), {1, 2, 3, 4, 5});
    return {};
}

sgrn::Result<void> DatastoreShell::cmdStorageOrphans(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->tryStorageAdminOrphans(shellFlag(t_args, "--bucket"), shellFlag(t_args, "--prefix"),
        shellUint(t_args, "--limit", 500), shellUint(t_args, "--max-pages", 10));
    RETURN_IF(r.hasError(), r.error());
    const auto& rep = r.value();
    fmt::print("bucket '{}'{}: scanned {} garage keys{} | garage-only {} ({} bytes) | db-missing {} ({} unchecked)\n", rep.bucket_,
        rep.prefix_.empty() ? "" : " prefix '" + rep.prefix_ + "'", rep.garage_scanned_, rep.garage_truncated_ ? " [truncated]" : "",
        rep.garage_only_count_, rep.garage_only_bytes_, rep.db_missing_.size(), rep.db_missing_unchecked_);
    if (!rep.garage_only_.empty()) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& k : rep.garage_only_)
            rows.push_back({k.key_, std::to_string(k.size_), k.etag_});
        printTable({"GARAGE-ONLY KEY (purge candidates)", "SIZE", "ETAG"}, std::move(rows), {1});
    }
    if (!rep.db_missing_.empty()) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& k : rep.db_missing_)
            rows.push_back({k.key_, std::to_string(k.size_)});
        printTable({"DB ROW WITHOUT GARAGE OBJECT (broken refs)", "SIZE"}, std::move(rows), {1});
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdStoragePurge(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!client_, "not connected");
    std::vector<std::string> keys;
    for (size_t i = 0; i < t_args.size(); ++i) {
        if (!t_args[i].empty() && t_args[i][0] != '-') {
            const bool is_value = i > 0 && (t_args[i - 1] == "--bucket" || t_args[i - 1] == "--prefix" || t_args[i - 1] == "--limit");
            if (!is_value)
                keys.push_back(t_args[i]);
        }
    }
    const bool dry_run = !shellHasFlag(t_args, "--execute");
    auto r = client_->tryStorageAdminPurge(
        shellFlag(t_args, "--bucket"), keys, shellFlag(t_args, "--prefix"), dry_run, shellUint(t_args, "--limit", 500));
    RETURN_IF(r.hasError(), r.error());
    const auto& res = r.value();
    if (res.dry_run_) {
        fmt::print("DRY RUN — {} key(s) would be deleted (re-run with --execute to arm):\n", res.affected_.size());
        std::vector<std::vector<std::string>> rows;
        for (const auto& k : res.affected_)
            rows.push_back({k});
        printTable({"WOULD DELETE"}, std::move(rows));
    } else {
        fmt::print(
            "deleted {} key(s), {} error(s), {} skipped (re-referenced):\n", res.affected_.size(), res.errors_.size(), res.skipped_.size());
        if (!res.affected_.empty()) {
            std::vector<std::vector<std::string>> rows;
            for (const auto& k : res.affected_)
                rows.push_back({k});
            printTable({"DELETED"}, std::move(rows));
        }
    }
    if (!res.skipped_.empty()) {
        for (const auto& k : res.skipped_)
            fmt::print("  skipped (now referenced): {}\n", k);
    }
    if (!res.errors_.empty()) {
        std::string joined;
        for (size_t i = 0; i < res.errors_.size(); ++i) {
            if (i > 0)
                joined += "\n";
            joined += res.errors_[i];
        }
        return ::sgrn::Error(std::move(joined));
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdStorageSearch(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!client_, "not connected");
    const std::string prefix_val = shellFlag(t_args, "--prefix");
    std::string target;
    for (size_t i = 0; i < t_args.size(); ++i) {
        if (!t_args[i].empty() && t_args[i][0] != '-') {
            const bool is_value = i > 0 && (t_args[i - 1] == "--bucket" || t_args[i - 1] == "--limit" || t_args[i - 1] == "--prefix");
            if (!is_value && target.empty())
                target = t_args[i];
        }
    }
    RETURN_ERROR_IF(!target.empty() && !prefix_val.empty(), "supply either a hash or --prefix, not both");
    RETURN_ERROR_IF(
        target.empty() && prefix_val.empty(), "missing object hash (usage: storage-search [--bucket B] <HASH> | --prefix PREFIX)");
    auto r = client_->storageAdminSearchJson(shellFlag(t_args, "--bucket"), target, prefix_val, shellUint(t_args, "--limit", 100));
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

} // namespace sgrn::datastore::shell
