#pragma once

#include <sgrn/Result.hpp>

#include <string>

namespace sgrn::gateway::twin
{
class PlcState;
}
namespace sgrn::scl
{
class PlcSchemaStore;
}

namespace sgrn::gateway::core
{

/**
 * @brief Outcome of a boot-time recovery from the WAL archives.
 *
 * leaves_skipped counts individual leaves for JSONL archives, and dropped
 * DBs/corrupt frames for binary archives (coarser unit — binary recovery
 * works on whole images, not leaves).
 */
struct RecoveryResult {
    int leaves_restored{0};
    int leaves_skipped{0};    ///< schema drift — see the PlcNode::find() null-check pattern
    std::string archive_used; ///< for the boot log line
};

/**
 * @brief Restore the digital twin from the newest matching WAL archive.
 *
 * Scans <state_dir>/unsynced and <state_dir>/synced for `.jsonl.zst` and
 * `.bin.zst` archives, newest first, and restores from the newest one whose
 * embedded schema matches t_schema_store:
 * - JSONL: seeks to its last anchor via the footer, decodes the anchor into
 *   t_state, then replays every delta line after it.
 * - Binary: single pass over frames into per-DB images (full frames replace,
 *   CRC-verified anchors replace, deltas patch), committed to the arena at
 *   EOF; a torn tail keeps whatever parsed.
 *
 * Mirrors the "no dirty events, no callbacks, bump version at the end"
 * contract that TreePersistenceStore::load() used to establish: values are
 * encoded directly into the arena under the per-segment lock, notifications
 * stay silent, and every DB-root version is bumped once at the end so
 * TreeCacheEngine rebuilds on first read.
 */
sgrn::Result<RecoveryResult, std::string> recoverStateFromArchives(
    const std::string& t_state_dir, twin::PlcState& t_state, const scl::PlcSchemaStore& t_schema_store);

} // namespace sgrn::gateway::core
