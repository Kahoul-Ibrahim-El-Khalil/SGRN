#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/core.h>

namespace sgrn::gateway::twin
{

LeafDictionary LeafDictionary::buildFrom(const scl::PlcSchemaStore& t_store) {
    LeafDictionary dict;
    LeafId next_id = 0;

    for (const auto& [db_num, schema] : t_store.dbs()) {
        visitDbFields(schema.fields, [&](const DbFieldVisitInfo& t_info) {
            if (t_info.is_leaf) {
                dict.id_to_path.push_back({next_id, t_info.path});
                dict.path_to_id[t_info.path] = next_id;
                dict.id_to_path_map[next_id] = t_info.path;
                ++next_id;
            }
        });
    }

    return dict;
}

sgrn::Result<void, std::string> expandRecordKeys(const rapidjson::Value& t_record,
    const std::unordered_map<LeafId, std::string>& t_id_to_path, rapidjson::Document::AllocatorType& t_alloc, rapidjson::Value& t_out) {

    if (!t_record.IsObject()) {
        return sgrn::Error("Record is not a JSON object");
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
                    auto path_it = t_id_to_path.find(id);
                    if (path_it != t_id_to_path.end()) {
                        rapidjson::Value new_key(
                            path_it->second.c_str(), static_cast<rapidjson::SizeType>(path_it->second.length()), t_alloc);
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

} // namespace sgrn::gateway::twin
