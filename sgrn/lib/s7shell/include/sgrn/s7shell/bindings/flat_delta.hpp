#pragma once
// =============================================================================
// flat_delta.hpp — flat leaf-id delta encoding shared by the s7shell server
// bindings (Gateway, WebSocketServer).
//
// This is the same wire form the full gateway emits in dictionary mode
// (GatewayApplication::wireTelemetry → PlcMemory::getDeltaSnapshotFlat):
//   {"<leaf_id>": value, ...}
// No per-DB grouping — paths (via their dictionary IDs) and their deltas
// suffice. broadcastDelta() forwards the blob verbatim to every client, and
// the adapter pushes the {"type":"dictionary","leaves":[...]} frame at
// connect time (see WebSocketAdapter onopen) so clients can decode IDs.
//
// PlcState dirty flags vs PlcRuntime ledger: direct writeDbMemory() marks
// the PlcState segment dirty, but command-queue writes only land (and mark)
// after processor()->processCommands(). Callers must flush first. When the
// PlcState sweep still comes back empty while the PlcRuntime ledger reports
// dirty DBs, we rebuild from per-DB JSON and flatten via the dictionary, so
// an explicit sync() always pushes the ledger state.
// =============================================================================

#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::s7shell::bindings
{

/// Flat id-keyed delta for the given dirty tags (TIA-style discrete tags,
/// addressed by name in the dictionary). Consumes the tag ledger, like
/// takeDirty() does for DB regions — only gateways consume it.
inline std::string flatDeltaForDirtyTags(
    const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& t_rt, const ::sgrn::gateway::twin::LeafDictionary& t_dict) {
    if (!t_rt)
        return "{}";
    const std::vector<std::string> names = t_rt->takeDirtyTags();
    if (names.empty())
        return "{}";
    std::string out = "{";
    bool first = true;
    for (const auto& name : names) {
        const auto id_it = t_dict.path_to_id.find(name);
        if (id_it == t_dict.path_to_id.end())
            continue; // cannot happen: tags are discrete areas only
        auto jr = t_rt->readTagJson(name);
        if (jr.hasError())
            continue;
        if (!first)
            out += ",";
        out += "\"" + std::to_string(id_it->second) + "\":" + jr.value();
        first = false;
    }
    out += "}";
    return first ? "{}" : out;
}

/// Flat id-keyed delta for the given dirty DBs plus all dirty tags.
/// Returns "{}" when there is nothing to send. Never consumes the DB dirty
/// ledger; the tag ledger IS consumed (take semantics, like takeDirty() —
/// only gateways consume it). An empty DB list still emits pending tags.
inline std::string flatDeltaForDirtyDbs(const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& t_rt,
    const ::sgrn::gateway::twin::LeafDictionary& t_dict, const std::vector<uint16_t>& t_dirty_dbs) {
    std::string db_part = "{}";
    if (t_rt && !t_dirty_dbs.empty() && !t_dict.path_to_id.empty()) {
        db_part = t_rt->getMemory().getDeltaSnapshotFlat(t_dict.path_to_id, t_dirty_dbs);
        if (db_part.empty() || db_part == "{}") {
            // Fallback: the PlcState sweep found nothing (e.g. flags were
            // cleared by an earlier read) while the ledger is still dirty —
            // rebuild nested per-DB JSON and flatten through the dictionary.
            std::string nested = "{";
            bool first = true;
            for (uint16_t db_num : t_dirty_dbs) {
                auto jr = t_rt->getMemory().getDbJson(db_num);
                if (jr.hasError())
                    continue;
                auto db_res = t_rt->getSchema().getDb(db_num);
                if (db_res.hasError() || !db_res.value())
                    continue;
                if (!first)
                    nested += ",";
                nested += "\"" + db_res.value()->db_name + "\":" + jr.value();
                first = false;
            }
            nested += "}";
            if (!first) {
                rapidjson::Document nested_doc;
                nested_doc.Parse(nested.c_str());
                if (!nested_doc.HasParseError() && nested_doc.IsObject()) {
                    rapidjson::Document flat_doc;
                    if (!::sgrn::gateway::twin::flattenNestedTree(nested_doc, t_dict.path_to_id, flat_doc.GetAllocator(), flat_doc)
                            .hasError()) {
                        rapidjson::StringBuffer sb;
                        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
                        flat_doc.Accept(writer);
                        const std::string out = sb.GetString();
                        if (!out.empty())
                            db_part = out;
                    }
                }
            }
        }
    }

    const std::string tag_part = flatDeltaForDirtyTags(t_rt, t_dict);
    if (db_part == "{}")
        return tag_part;
    if (tag_part == "{}")
        return db_part;
    return db_part.substr(0, db_part.size() - 1) + "," + tag_part.substr(1);
}

/// Collect the currently-dirty DB numbers from the PlcRuntime ledger.
inline std::vector<uint16_t> collectDirtyDbs(const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& t_rt) {
    std::vector<uint16_t> dirty;
    if (!t_rt)
        return dirty;
    for (const auto& [db_num, _] : t_rt->getSchema().dbs()) {
        if (t_rt->hasDirty(db_num))
            dirty.push_back(db_num);
    }
    return dirty;
}

} // namespace sgrn::s7shell::bindings
