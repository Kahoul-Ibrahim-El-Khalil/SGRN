#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/core.h>

using sgrn::Result;
namespace sgrn::gateway::twin
{

LeafDictionary LeafDictionary::buildFrom(const scl::PlcSchemaStore& t_store) {
    LeafDictionary dict;
    LeafId next_id = 0;

    for (const auto& [db_num, schema] : t_store.dbs()) {
        // Match the naming convention PlcState::registerSegment() already uses
        // (see PlcState.cpp:27118) — dictionary paths must agree with the actual
        // top-level segment/JSON key names, or every lookup silently misses.
        const std::string db_prefix = schema.db_name.empty() ? fmt::format("DB{}", db_num) : schema.db_name;

        visitDbFields(schema.fields, [&](const DbFieldVisitInfo& t_info) {
            if (t_info.is_leaf) {
                const std::string full_path = db_prefix + "." + t_info.path;
                dict.path_by_id.push_back(full_path);
                dict.path_to_id[full_path] = next_id;
                ++next_id;
            }
        });
    }

    return dict;
}
Result<void, std::string> expandRecordKeys(const rapidjson::Value& t_record, const std::vector<std::string>& t_path_by_id,
    rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out) {

    if (!t_record.IsObject()) {
        return Error("Record is not a JSON object");
    }

    t_out.SetObject();

    const rapidjson::Value* payload = nullptr;
    std::string payload_key;
    if (t_record.HasMember("data") && t_record["data"].IsObject()) {
        payload = &t_record["data"];
        payload_key = "data";
    } else if (t_record.HasMember("changes") && t_record["changes"].IsObject()) {
        payload = &t_record["changes"];
        payload_key = "changes";
    }

    // Copy all members to t_out
    for (auto it = t_record.MemberBegin(); it != t_record.MemberEnd(); ++it) {
        std::string key_str = it->name.GetString();
        if (payload && key_str == payload_key) {
            rapidjson::Value expanded_payload(rapidjson::kObjectType);
            for (auto p_it = payload->MemberBegin(); p_it != payload->MemberEnd(); ++p_it) {
                std::string p_key_str = p_it->name.GetString();
                try {
                    LeafId id = static_cast<LeafId>(std::stoul(p_key_str));
                    if (id < t_path_by_id.size()) {
                        rapidjson::Value new_key(
                            t_path_by_id[id].c_str(), static_cast<rapidjson::SizeType>(t_path_by_id[id].length()), t_alloc);
                        rapidjson::Value new_value;
                        new_value.CopyFrom(p_it->value, t_alloc);
                        expanded_payload.AddMember(new_key, new_value, t_alloc);
                    } else {
                        rapidjson::Value new_key;
                        new_key.CopyFrom(p_it->name, t_alloc);
                        rapidjson::Value new_value;
                        new_value.CopyFrom(p_it->value, t_alloc);
                        expanded_payload.AddMember(new_key, new_value, t_alloc);
                    }
                } catch (const std::exception&) {
                    rapidjson::Value new_key;
                    new_key.CopyFrom(p_it->name, t_alloc);
                    rapidjson::Value new_value;
                    new_value.CopyFrom(p_it->value, t_alloc);
                    expanded_payload.AddMember(new_key, new_value, t_alloc);
                }
            }
            rapidjson::Value new_main_key(payload_key.c_str(), static_cast<rapidjson::SizeType>(payload_key.length()), t_alloc);
            t_out.AddMember(new_main_key, expanded_payload, t_alloc);
        } else {
            rapidjson::Value new_key;
            new_key.CopyFrom(it->name, t_alloc);
            rapidjson::Value new_value;
            new_value.CopyFrom(it->value, t_alloc);
            t_out.AddMember(new_key, new_value, t_alloc);
        }
    }

    return {};
}

// ── flattenNestedTree ────────────────────────────────────────────────────────
// Internal recursive helper that walks a nested JSON object and emits flat
// id-keyed entries for every leaf whose dotted path resolves in path_to_id.
namespace
{
void flattenWalk(const rapidjson::Value& t_node, const std::string& t_prefix,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, const std::vector<bool>* t_allowed,
    rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out) {
    if (t_node.IsObject()) {
        for (auto it = t_node.MemberBegin(); it != t_node.MemberEnd(); ++it) {
            std::string child_path = t_prefix.empty() ? it->name.GetString() : t_prefix + "." + it->name.GetString();
            flattenWalk(it->value, child_path, t_path_to_id, t_allowed, t_alloc, t_out);
        }
    } else if (t_node.IsArray()) {
        // First check if this prefix is itself a dictionary leaf (array-typed field).
        // If so, emit the whole array value as-is rather than descending with [i] suffixes.
        auto id_it = t_path_to_id.find(t_prefix);
        if (id_it != t_path_to_id.end()) {
            if (t_allowed && id_it->second < t_allowed->size() && !(*t_allowed)[id_it->second])
                return;
            std::string id_str = std::to_string(id_it->second);
            rapidjson::Value key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()), t_alloc);
            rapidjson::Value val;
            val.CopyFrom(t_node, t_alloc);
            t_out.AddMember(key, val, t_alloc);
            return;
        }
        // Not a leaf: descend into elements (for nested array-of-struct schemas).
        for (rapidjson::SizeType i = 0; i < t_node.Size(); ++i) {
            std::string elem_path = t_prefix + "[" + std::to_string(i) + "]";
            flattenWalk(t_node[i], elem_path, t_path_to_id, t_allowed, t_alloc, t_out);
        }
    } else {
        // Leaf value — look up the dotted path in the dictionary.
        auto id_it = t_path_to_id.find(t_prefix);
        SGRN_RETURN_IF(id_it == t_path_to_id.end(), ;);

        SGRN_RETURN_IF(t_allowed && id_it->second < t_allowed->size() && !(*t_allowed)[id_it->second], ;);

        std::string id_str = std::to_string(id_it->second);

        rapidjson::Value key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()), t_alloc);
        rapidjson::Value val;
        val.CopyFrom(t_node, t_alloc);
        t_out.AddMember(key, val, t_alloc);
    }
}
} // anonymous namespace

Result<void, std::string> flattenNestedTree(const rapidjson::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, rapidjson::Document::AllocatorType& t_alloc,
    rapidjson::Value& t_out) {
    SGRN_RETURN_ERROR_IF(!t_nested.IsObject(), "flattenNestedTree: input is not a JSON object");
    t_out.SetObject();
    flattenWalk(t_nested, "", t_path_to_id, nullptr, t_alloc, t_out);
    return {};
}

Result<void, std::string> flattenNestedTreeFiltered(const rapidjson::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, const std::vector<bool>& t_allowed,
    rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out) {

    SGRN_RETURN_ERROR_IF(!t_nested.IsObject(), "flattenNestedTreeFiltered: input is not a JSON object");

    t_out.SetObject();
    flattenWalk(t_nested, "", t_path_to_id, &t_allowed, t_alloc, t_out);
    return {};
}

} // namespace sgrn::gateway::twin
