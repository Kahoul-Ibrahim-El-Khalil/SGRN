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
 * @brief Outcome of a boot-time recovery from the JSONL WAL archives.
 */
struct RecoveryResult {
    int leaves_restored{0};
    int leaves_skipped{0};    ///< schema drift — see the PlcNode::find() null-check pattern
    std::string archive_used; ///< for the boot log line
};

/**
 * @brief Restore the digital twin from the newest matching JSONL WAL archive.
 *
 * Scans <state_dir>/unsynced and <state_dir>/synced for .jsonl.zst archives,
 * newest first, opens the newest one whose embedded schema line matches
 * t_schema_store, seeks to its last anchor via the footer, decodes the anchor
 * into t_state, then replays every delta line after it.
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
