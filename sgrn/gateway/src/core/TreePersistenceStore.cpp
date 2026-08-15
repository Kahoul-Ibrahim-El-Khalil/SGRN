#include <fmt/core.h>
#include <sgrn/gateway/core/TreePersistenceStore.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/utils/time.hpp>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <shared_mutex>

namespace sgrn::gateway::core
{

// ── Save ─────────────────────────────────────────────────────────────────────

bool TreePersistenceStore::save(const twin::PlcState& t_state) const {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();

    int written = 0;
    for (const auto& [dotted_path, p_node] : t_state.nodes()) {
        // Only persist leaf nodes — structs/arrays are reconstructed from leaves
        if (!p_node.children_.empty())
            continue;
        if (!p_node.cached_slot_)
            continue;

        // Use the fast scalar path (avoids full RapidJSON serialize pipeline)
        std::string value = t_state.getScalarString(dotted_path);
        if (value.empty() || value == "null")
            continue;

        writer.Key(dotted_path.c_str(), static_cast<rapidjson::SizeType>(dotted_path.size()));
        // RawValue works for both primitives (100.0, true) and strings ("\"foo\"")
        writer.RawValue(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), rapidjson::kNumberType);
        ++written;
    }

    writer.EndObject();

    std::ofstream out(path_, std::ios::trunc);
    if (!out.is_open()) {
        fmt::print(stderr, "[persistence] Cannot open {} for writing\n", path_);
        return false;
    }
    out << sb.GetString();
    fmt::print("[persistence] Saved {} leaf values → {}\n", written, path_);
    return true;
}

// ── Load ─────────────────────────────────────────────────────────────────────

int TreePersistenceStore::load(twin::PlcState& t_state) const {
    std::ifstream in(path_);
    if (!in.is_open())
        return -1; // Normal on first boot

    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty())
        return -1;

    rapidjson::Document doc;
    if (doc.Parse(json.c_str()).HasParseError() || !doc.IsObject()) {
        fmt::print(stderr, "[persistence] Corrupt snapshot at {}, ignoring.\n", path_);
        return -1;
    }

    int restored = 0;
    int skipped = 0;

    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const std::string dotted_path = it->name.GetString();

        const twin::PlcNode* p_node = t_state.find(dotted_path);
        if (!p_node || !p_node->cached_slot_ || !p_node->children_.empty()) {
            ++skipped; // schema changed since last save — silently skip
            continue;
        }

        // Build the minimal DbField descriptor the encoder needs
        twin::DbField field;
        field.name = p_node->name_;
        field.type = p_node->type_;
        field.count = static_cast<int>(p_node->count_);
        field.bit_index = static_cast<int>(p_node->bit_index_);
        field.endianness = p_node->endian_;
        field.offset = 0;

        // Re-serialize the stored RapidJSON value into a compact JSON string
        rapidjson::StringBuffer vsb;
        rapidjson::Writer<rapidjson::StringBuffer> vw(vsb);
        it->value.Accept(vw);
        const std::string value_json = vsb.GetString();

        auto* p_entry = const_cast<twin::DbEntry*>(p_node->cached_slot_);
        const size_t node_start = p_node->offset_;
        uint8_t* p_ptr = t_state.arenaData() + p_entry->offset + node_start;
        const size_t buf_remaining = p_entry->size - node_start;

        {
            std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);
            auto res = twin::encodeFieldAt(field, value_json, p_ptr, buf_remaining, 0, p_node->endian_);
            if (res.hasError()) {
                ++skipped;
                continue;
            }
        }
        // Silently bump version so TreeCacheEngine marks this path stale.
        // No dirty flag, no event publish — adapters haven't started yet.
        t_state.incrementNodeVersion(twin::TreePath::fromDotted(dotted_path));
        ++restored;
    }

    // Bump every DB-root version so the cache knows the tree has changed
    for (const auto& name_ : t_state.getTopLevelNames()) {
        t_state.incrementNodeVersion(twin::TreePath::fromDotted(name_));
    }

    fmt::print(
        "[persistence] Restored {} / {} leaves from {} ({} schema-mismatches skipped)\n", restored, restored + skipped, path_, skipped);
    return restored;
}

} // namespace sgrn::gateway::core
