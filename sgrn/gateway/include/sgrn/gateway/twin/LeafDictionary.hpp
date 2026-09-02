#pragma once

#include <sgrn/Result.hpp>
#include <rapidjson/document.h>

#include <ankerl/unordered_dense.h>
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
    ankerl::unordered_dense::map<std::string, LeafId> path_to_id; // for the write path (fast encode)
    std::vector<std::string> path_by_id;                          // dense array for the read path (fast decode)

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
sgrn::Result<void, std::string> expandRecordKeys(const rapidjson::Value& t_record, const std::vector<std::string>& t_path_by_id,
    rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out);

/**
 * @brief Flattens a nested JSON tree into a flat id-keyed map using path_to_id.
 *
 * Walks a nested object like {"ReactorCore": {"thermal_power_mw": 100.0}} and
 * produces {"0": 100.0} where 0 is the leaf ID for "ReactorCore.thermal_power_mw".
 * Array elements are flattened with "[i]" suffix in the path lookup.
 * Leaves whose path is not in path_to_id are skipped.
 */
sgrn::Result<void, std::string> flattenNestedTree(const rapidjson::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, rapidjson::Document::AllocatorType& t_alloc,
    rapidjson::Value& t_out);

/**
 * @brief Like flattenNestedTree, but also applies a leaf-ID filter.
 *
 * Only leaves with t_allowed[id] == true are emitted. t_allowed must be
 * pre-built to cover all IDs in the dictionary.
 */
sgrn::Result<void, std::string> flattenNestedTreeFiltered(const rapidjson::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, const std::vector<bool>& t_allowed,
    rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out);

} // namespace sgrn::gateway::twin
