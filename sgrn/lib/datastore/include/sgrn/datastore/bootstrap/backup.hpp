#pragma once
// sgrn/lib/datastore/include/sgrn/datastore/bootstrap/backup.hpp
//
// `--backup-db` and `--backup-garage` — separate, atomic, self-describing
// artifacts (see help.hpp). Each command:
//
//   1. writes its artifact to a .tmp file inside <base_dir>/backup/
//   2. verifies/finalizes it (pg_restore --list, per-object sha256)
//   3. renames the .tmp to its final name in ONE atomic step
//   4. writes a manifest.json next to it (digests, counts, provenance)
//
// A crash leaves at most an orphan .tmp: the final artifact either exists
// complete or not at all. Stale .tmp files are cleaned on the next run.
//
// Backups are read-only against the deployment: pg_dump (a separate
// process) and S3 GET requests only. Secrets: none of the rotating keys
// are echoed; the manifest carries digests/counts, never data.
//
// Placement rules (decided):
//   - artifacts are CATALOG-DRIVEN: --backup-garage enumerates
//     storage.objects and fetches exactly those (bucket, sha256) pairs.
//     Objects in Garage without a catalog row (orphans, e.g. an upload
//     that committed bytes before dying) are NOT carried; that is the
//     safe asymmetry (missing bytes are loud, extra bytes are harmless).
//   - sha256 of stored bytes must match the catalog row; discrepancies
//     are skipped, counted and recorded in the manifest (failures stay
//     visible without destroying an otherwise-complete archive).
//   - consistency: run with the datastore STOPPED (plain best-effort
//     here — print a reminder; a systemd-timer wrapper enforces it later).
//
// Garage side: plain ustar tar, NO compression (content is codec- or
// object-level compressed already — see the formats table). Postgres side:
// pg_dump -Fc -Z9 (natively compressed SQL, keeps pg_restore semantics).
// Neither artifact is ever outer-compressed here.
//
// This header carries the interface only (plus the sgrn::utils re-exports
// the tests use); definitions live in src/bootstrap/backup.cpp.

#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/process.hpp>
#include <sgrn/utils/progress.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/tar.hpp>
#include <sgrn/utils/time.hpp>
#include <filesystem>
#include <string>

namespace sgrn::datastore::bootstrap::backup
{
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// imported utilities from sgrn::utils
// ---------------------------------------------------------------------------

using sgrn::utils::ProgressMeter;
using sgrn::utils::Sha256Stream;
using sgrn::utils::filesystem::ensureOwnerOnlyPerms;
using sgrn::utils::process::machineName;
using sgrn::utils::strings::formatBytes;
using sgrn::utils::strings::jsonEscapeInto;
using sgrn::utils::tar::kTarBlockSize;
using sgrn::utils::tar::TarWriter;
using sgrn::utils::time::formatDuration;
using sgrn::utils::time::utcStamp;

// ---------------------------------------------------------------------------
// atomic-artifact primitives
// ---------------------------------------------------------------------------

/// Create <base_dir>/backup/ and clean orphan .tmp files from interrupted runs.
fs::path prepareBackupDir(const std::string& t_base_dir);

/// Atomic cutover: fsync-free best effort, but the rename itself is atomic
/// on POSIX — the final name is either absent or points at a complete file.
bool publishArtifact(const fs::path& t_tmp, const fs::path& t_final);

// ---------------------------------------------------------------------------
// --backup-db : pg_dump -Fc -Z9, atomic, verified, manifest
// ---------------------------------------------------------------------------

int backupDatabase(const std::string& t_base_dir);

// ---------------------------------------------------------------------------
// --backup-garage : catalog-driven object drain into a plain ustar tar
// ---------------------------------------------------------------------------

int backupGarage(const std::string& t_base_dir);

// ---------------------------------------------------------------------------
// --verify-backup : cross-verify backup archive(s) against PostgreSQL catalog
// ---------------------------------------------------------------------------

int verifyBackup(const std::string& t_base_dir, const std::string& t_target = "");

// ---------------------------------------------------------------------------
// --backup-keys : SQLite online backup of keys.db (mode 0600, separate storage)
// ---------------------------------------------------------------------------

int backupKeyDb(const std::string& t_base_dir, const std::string& t_target = "");

// ---------------------------------------------------------------------------
// --verify-keys : Cross-reconcile SQLite key records with PostgreSQL files
// ---------------------------------------------------------------------------

int verifyKeys(const std::string& t_base_dir);

} // namespace sgrn::datastore::bootstrap::backup
