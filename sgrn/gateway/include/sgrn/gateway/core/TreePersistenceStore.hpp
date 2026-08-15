#pragma once

#include <string>

namespace sgrn::gateway::twin
{
class PlcState;
}

namespace sgrn::gateway::core
{

/**
 * @brief Persistent leaf-level snapshot store for the digital twin.
 *
 * DESIGN
 * ──────
 * Save:  Walk PlcState::nodes(), collect every leaf's scalar JSON value via
 *        getScalarString(), write a compact flat map { "DB.path": "value" }.
 *        No dirty flags consumed, no arena locks held beyond the per-leaf read.
 *
 * Load:  For each (dotted_path, json_value) pair, directly encode the value
 *        into the arena using the same encodeFieldAt() path as PlcCommandProcessor,
 *        but WITHOUT marking dirty, WITHOUT firing field-update callbacks,
 *        and WITHOUT calling signalDirty().
 *        After all leaves are written, bump the version counter on every
 *        DB-root node so that TreeCacheEngine treats them as stale and will
 *        re-build on first read.
 *
 * This means the gateway boots with last-known state visible via HTTP and
 * WebSocket immediately, with zero startup broadcast noise.
 */
class TreePersistenceStore {
public:
    explicit TreePersistenceStore(const std::string& t_path)
        : path_(t_path) {
    }

    /// Persist all leaf values from state to disk. Non-blocking, called async.
    bool save(const twin::PlcState& t_state) const;

    /// Restore leaf values directly into the arena. Call before any adapters start.
    /// Returns number of leaves restored, or -1 on file/parse error.
    int load(twin::PlcState& t_state) const;

private:
    std::string path_;
};

} // namespace sgrn::gateway::core
