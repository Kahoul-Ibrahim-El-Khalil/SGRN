#include <fmt/core.h>
#include <sgrn/gateway/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/core/snapshot.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <algorithm>

namespace sgrn::gateway::core
{

std::string buildSnapshotJson(sgrn::gateway::twin::PlcMemory& t_server,
    const std::map<uint16_t, const ::sgrn::scl::DataBlockRegistry*>& t_regs,
    const std::unordered_map<uint16_t, std::vector<std::pair<int32_t, int32_t>>>& t_filter) {

    if (t_filter.empty()) {
        return t_server.getDigitalTwinJsonString();
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

    writer.StartObject();
    for (const auto& [db, ranges] : t_filter) {
        auto it_reg = t_regs.find(db);
        if (it_reg == t_regs.end() || !it_reg->second)
            continue;
        const auto* reg = it_reg->second;

        auto* state = t_server.state();
        if (!state)
            continue;

        auto* db_entry = state->findSegmentById(db);
        if (!db_entry)
            continue;

        std::shared_lock<std::shared_mutex> lock(db_entry->mutex_);
        const uint8_t* buf = state->arenaData() + db_entry->offset;
        size_t b_size = db_entry->size;

        const std::string key = reg->db_name.empty() ? fmt::format("DB{}", db) : reg->db_name;
        writer.Key(key.c_str(), static_cast<rapidjson::SizeType>(key.length()));

        writer.StartObject();
        for (const auto& field : reg->fields) {
            int f_size = (field.type == ::sgrn::scl::DataType::Bool) ? 1 : s7codec::typeSpanBytes(field.type, field.count);
            if (!field.children.empty() && field.struct_size > 0)
                f_size = field.struct_size;

            bool overlaps = false;
            for (const auto& r : ranges) {
                if (field.offset < r.second && (field.offset + f_size) > r.first) {
                    overlaps = true;
                    break;
                }
            }

            if (overlaps) {
                writer.Key(field.name.c_str(), static_cast<rapidjson::SizeType>(field.name.length()));
                sgrn::gateway::adapters::s7::serializeComplexFieldTo(writer, field, buf, b_size, ranges);
            }
        }
        writer.EndObject();
    }
    writer.EndObject();

    return sb.GetString();
}

} // namespace sgrn::gateway::core
