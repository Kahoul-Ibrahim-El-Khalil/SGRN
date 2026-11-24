// cmd-versions.cpp — file version history: list, download, restore, prune.
//
// Mirrors the web dashboard's version panel (GET/DELETE /storage/files/versions,
// POST /storage/files/versions/restore, GET /storage/files?version=N). The
// endpoints require a user session, so these commands need `login`/`connect
// --email`; service credentials are rejected by the server.
#include "../shell-common.hpp"

namespace sgrn::datastore::shell
{

namespace
{

/// Parses a positive whole number, accepting a leading 'v' ("3" or "v3").
Result<int32_t> parsePositiveInt(const std::string& t_text, const char* t_what) {
    std::string digits = t_text;
    if (!digits.empty() && (digits[0] == 'v' || digits[0] == 'V'))
        digits.erase(0, 1);

    const bool all_digits =
        !digits.empty() && std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    RETURN_ERROR_IF(!all_digits || digits.size() > 9, fmt::format("'{}': {} must be a positive whole number", t_text, t_what));

    const int32_t value = std::stoi(digits);
    RETURN_ERROR_IF(value < 1, fmt::format("'{}': {} must be at least 1", t_text, t_what));
    return value;
}

/// "report.txt" + 3 -> "report.v3.txt"; names without an extension get ".v3" appended
/// (same naming as the dashboard's version download).
std::string versionedFileName(const std::string& t_name, int32_t t_version_no) {
    const size_t dot = t_name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return fmt::format("{}.v{}", t_name, t_version_no);
    return fmt::format("{}.v{}{}", t_name.substr(0, dot), t_version_no, t_name.substr(dot));
}

/// "current", "delta", "full" or "checkpoint", plus "restore" when the version was made by a restore.
std::string describeVersionState(const sgrn::datastore::client::FileVersionInfo& t_version) {
    std::string state;
    if (t_version.is_current)
        state = "current";
    else if (t_version.storage_kind == "delta")
        state = "delta";
    else
        state = t_version.is_checkpoint ? "checkpoint" : "full";

    if (t_version.restored_from_version_id.has_value())
        state += ",restore";
    return state;
}

} // namespace

Result<void> DatastoreShell::cmdVersions(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto operands = shellPositionals(t_args);
    RETURN_ERROR_IF(operands.empty(), "missing file operand (versions <FILE>)");

    const std::string path = normalizePath(operands[0]);
    auto r = storage_->listVersions(path, scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", path, r.error()));

    const auto& versions = r.value();
    if (versions.empty()) {
        fmt::print("{}: no versions\n", path);
        return {};
    }

    int64_t stored_total = 0;
    int64_t logical_total = 0;
    std::vector<std::vector<std::string>> rows;
    for (const auto& v : versions) {
        stored_total += v.stored_size;
        logical_total += v.original_size;
        rows.push_back({fmt::format("v{}", v.version_no), describeVersionState(v), humanBytes(v.original_size), humanBytes(v.stored_size),
            v.is_encrypted ? "yes" : "-", v.created_at, v.comment.value_or("")});
    }
    printTable({"VER", "STATE", "SIZE", "STORED", "ENC", "CREATED", "COMMENT"}, std::move(rows), {2, 3});

    if (versions.size() > 1) {
        const int saved =
            logical_total > stored_total ? static_cast<int>((1.0 - static_cast<double>(stored_total) / logical_total) * 100.0 + 0.5) : 0;
        fmt::print("{} versions · {} stored for {} of history{}\n", versions.size(), humanBytes(stored_total), humanBytes(logical_total),
            saved > 0 ? fmt::format(" ({}% saved by deltas)", saved) : "");
    }
    return {};
}

Result<void> DatastoreShell::cmdVersionGet(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto operands = shellPositionals(t_args);
    RETURN_ERROR_IF(operands.size() < 2, "usage: version-get <FILE> <VERSION> [LOCAL]");

    const std::string remote = normalizePath(operands[0]);
    auto version_no = parsePositiveInt(operands[1], "version");
    RETURN_IF(version_no.hasError(), version_no.error());

    const std::string default_name = versionedFileName(baseName(remote), version_no.value());
    std::string local = operands.size() > 2 ? expandLocalPath(operands[2]) : default_name;
    if (std::filesystem::is_directory(local))
        local = (std::filesystem::path(local) / default_name).string();

    const auto outcome = storage_->downloadVersion(remote, version_no.value(), local, scope_);
    RETURN_ERROR_IF(!outcome.ok, fmt::format("{} (v{}): {}", remote, version_no.value(), outcome.detail));
    return {};
}

Result<void> DatastoreShell::cmdVersionRestore(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto operands = shellPositionals(t_args, {"--comment"});
    RETURN_ERROR_IF(operands.size() < 2, "usage: version-restore <FILE> <VERSION> [--comment TEXT]");

    const std::string path = normalizePath(operands[0]);
    auto version_no = parsePositiveInt(operands[1], "version");
    RETURN_IF(version_no.hasError(), version_no.error());

    auto r = storage_->restoreVersion(path, version_no.value(), scope_, shellFlag(t_args, "--comment"));
    RETURN_IF(r.hasError(), fmt::format("{} (v{}): {}", path, version_no.value(), r.error()));

    const auto& receipt = r.value();
    if (receipt.created)
        fmt::print("{}: restored v{} as new current version v{}\n", path, version_no.value(), receipt.version_no);
    else
        fmt::print("{}: v{} already matches current version v{}; nothing new was created\n", path, version_no.value(), receipt.version_no);
    return {};
}

Result<void> DatastoreShell::cmdVersionPrune(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto operands = shellPositionals(t_args, {"--keep"});
    RETURN_ERROR_IF(operands.empty(), "usage: version-prune <FILE> --keep N [--execute]");

    const std::string keep_raw = shellFlag(t_args, "--keep");
    RETURN_ERROR_IF(keep_raw.empty(), "missing --keep N (the number of newest versions to retain, at least 1)");
    auto keep = parsePositiveInt(keep_raw, "--keep");
    RETURN_IF(keep.hasError(), keep.error());

    const std::string path = normalizePath(operands[0]);
    auto listing = storage_->listVersions(path, scope_);
    RETURN_IF(listing.hasError(), fmt::format("{}: {}", path, listing.error()));

    // Newest first; everything after the first `keep` entries is a prune candidate.
    auto versions = listing.value();
    std::sort(versions.begin(), versions.end(), [](const auto& t_a, const auto& t_b) { return t_a.version_no > t_b.version_no; });

    const size_t keep_count = static_cast<size_t>(keep.value());
    if (versions.size() <= keep_count) {
        fmt::print("{}: {} version(s), keeping the newest {}: nothing to prune\n", path, versions.size(), keep_count);
        return {};
    }

    if (!shellHasFlag(t_args, "--execute")) {
        fmt::print(
            "DRY RUN — up to {} of {} version(s) of {} would be deleted permanently (newest {} kept; re-run with --execute to arm):\n",
            versions.size() - keep_count, versions.size(), path, keep_count);
        std::vector<std::vector<std::string>> rows;
        for (size_t i = keep_count; i < versions.size(); ++i)
            rows.push_back({fmt::format("v{}", versions[i].version_no), humanBytes(versions[i].original_size), versions[i].created_at});
        printTable({"WOULD DELETE", "SIZE", "CREATED"}, std::move(rows), {1});
        return {};
    }

    auto r = storage_->trimVersions(path, keep.value(), scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", path, r.error()));
    fmt::print("{}: removed {} version(s), kept the newest {}\n", path, r.value().removed, keep_count);
    return {};
}

} // namespace sgrn::datastore::shell
