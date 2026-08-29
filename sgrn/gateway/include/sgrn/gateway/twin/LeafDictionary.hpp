#pragma once

#include <sgrn/Result.hpp>
#include <rapidjson/document.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::scl
{
class PlcSchemaStore;
}

namespace sgrn::gateway::twin
{

using LeafId = uint32_t;

/**
 * @brief Manages ID interning for twin telemetry leaves.
 *
 * This provides a memory-efficient mapping from dotted paths (e.g. "DB1.field")
 * to contiguous integer IDs.
 */
struct LeafDictionary {
    std::vector<std::pair<LeafId, std::string>> id_to_path; // ordered, for writing the dictionary line
    std::unordered_map<std::string, LeafId> path_to_id;     // for the write path (fast encode)
    std::unordered_map<LeafId, std::string> id_to_path_map; // for the read path (fast decode)

    /// Builds the dictionary in a fixed order: DBs ascending by db_number,
    /// fields within a DB in visitDbFields()'s declaration order. This
    /// function is the single source of truth for ID assignment — both
    /// PersistenceService (write) and the offline dump tool (read, as a
    /// fallback cross-check only) must call this, never hand-roll their
    /// own traversal.
    ///
    /// The dictionary line written into an archive is the *only*
    /// authoritative source for that archive's ID mapping. RecoveryEngine and
    /// the export tool must read it from the file, never recompute it from the live schema and
    /// assume it matches — a schema edit that reorders DbField::children would
    /// silently reassign every ID for every archive written before that edit.
    static LeafDictionary buildFrom(const scl::PlcSchemaStore& t_store);
};

/**
 * @brief Resolves a delta/anchor record's numeric IDs back to dotted paths.
 *
 * Takes a rapidjson::Value (a changes/data object), an id_to_path mapping,
 * and an allocator. Writes the resolved dotted-path keyed object into t_out.
 */
sgrn::Result<void, std::string> expandRecordKeys(const rapidjson::Value& t_record,
    const std::unordered_map<LeafId, std::string>& t_id_to_path, rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out);

} // namespace sgrn::gateway::twin
