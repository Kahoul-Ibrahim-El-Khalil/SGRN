#include <sgrn/gateway/adapters/opcua/delta_push.hpp>
#include <sgrn/gateway/adapters/opcua/json_to_ua.hpp>
#include <sgrn/gateway/adapters/opcua/memory_to_ua.hpp>
#include <sgrn/gateway/adapters/opcua/s7_to_ua.hpp>
#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <fmt/core.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace sgrn::gateway::twin;

namespace sgrn::gateway::adapters
{

using wrappers::opcua::DataValue;

DeltaPushHandler::DeltaPushHandler(wrappers::opcua::Server* tp_server)
    : server_(tp_server) {
}

void DeltaPushHandler::registerNode(const std::string& t_map_key, const wrappers::opcua::NodeId& t_node_id, const NodeContext* tp_ctx) {
    nodes_.insert_or_assign(t_map_key, RegisteredNode{wrappers::opcua::nodeIdFromRaw(t_node_id.get()), tp_ctx});
}

void DeltaPushHandler::setRunning(bool t_running) {
    running_.store(running_, std::memory_order_release);
}

void DeltaPushHandler::enqueueDataValue(const wrappers::opcua::NodeId& t_node_id, UA_DataValue&& t_raw_dv, uint64_t t_timestamp_ms) {
    t_raw_dv.hasSourceTimestamp = true;
    t_raw_dv.sourceTimestamp = wrappers::opcua::millisToUaDateTime(t_timestamp_ms);
    t_raw_dv.hasServerTimestamp = true;
    t_raw_dv.serverTimestamp = UA_DateTime_now();

    if (pending_write_) {
        pending_write_(wrappers::opcua::nodeIdFromRaw(t_node_id.get()), DataValue::adopt(std::move(t_raw_dv)));
        return;
    }
    if (server_)
        server_->writeDataValue(t_node_id, DataValue::adopt(std::move(t_raw_dv)));
}

bool DeltaPushHandler::buildDataValueFromEvent(const core::TelemetryEvent& t_event, const NodeContext* tp_ctx, UA_DataValue& t_raw_dv) {
    if (t_event.typed_leaf.bytes && t_event.typed_leaf.meta.valid && tp_ctx) {
        NodeContext typed_ctx = *tp_ctx;
        typed_ctx.type = static_cast<::sgrn::scl::DataType>(t_event.typed_leaf.meta.type);
        typed_ctx.field_size = t_event.typed_leaf.meta.field_size;
        typed_ctx.array_length = t_event.typed_leaf.meta.array_length;
        typed_ctx.elem_ua_type_index = t_event.typed_leaf.meta.elem_ua_type_index;
        if (s7BytesToDataValue(&typed_ctx, t_event.typed_leaf.bytes->data(), t_event.typed_leaf.bytes->size(), t_raw_dv))
            return true;
    }

    if (!t_event.json_value)
        return false;

    rapidjson::Document jv;
    if (jv.Parse(t_event.json_value->c_str()).HasParseError())
        return false;

    if (jv.IsArray()) {
        const int ua_type_idx = tp_ctx ? (tp_ctx->elem_ua_type_index >= 0 ? tp_ctx->elem_ua_type_index : UA_TYPES_DOUBLE) : UA_TYPES_DOUBLE;
        return jsonArrayToDataValue(jv, ua_type_idx, t_raw_dv);
    }
    return jsonValueToDataValue(jv, t_raw_dv);
}

void DeltaPushHandler::onTelemetryEvent(const core::TelemetryEvent& t_event) {
    if (!running_.load(std::memory_order_acquire))
        return;

    if (t_event.type == core::EventType::LeafUpdate) {
        const std::string map_key = fmt::format("{}:{}", t_event.db, t_event.path);
        auto node_it = nodes_.find(map_key);
        if (node_it == nodes_.end())
            return;

        const NodeContext* p_node_ctx = node_it->second.ctx;

        if (p_node_ctx && p_node_ctx->trigger_events && alarm_callback_ && t_event.json_value) {
            rapidjson::Document jv;
            if (!jv.Parse(t_event.json_value->c_str()).HasParseError()) {
                if (jv.IsObject()) {
                    alarm_callback_(t_event.db, t_event.path, jv, t_event.timestamp);
                } else if (jv.IsArray()) {
                    for (rapidjson::SizeType i = 0; i < jv.Size(); ++i)
                        alarm_callback_(t_event.db, fmt::format("{}[{}]", t_event.path, i), jv[i], t_event.timestamp);
                } else {
                    alarm_callback_(t_event.db, t_event.path, jv, t_event.timestamp);
                }
            }
        }

        UA_DataValue t_raw_dv{};
        if (!buildDataValueFromEvent(t_event, p_node_ctx, t_raw_dv))
            return;

        enqueueDataValue(node_it->second.node_id, std::move(t_raw_dv), t_event.timestamp);
        notifyAggregateAncestors(t_event.db, t_event.path, t_event.timestamp);
    }
}

void DeltaPushHandler::flattenAndPush(
    uint16_t t_db_number, const rapidjson::Value& t_obj, const std::string& t_path_prefix, uint64_t t_timestamp_ms) {
    if (!t_obj.IsObject())
        return;
    for (auto it = t_obj.MemberBegin(); it != t_obj.MemberEnd(); ++it) {
        std::string field_name = it->name.GetString();
        std::string full_path = t_path_prefix.empty() ? field_name : t_path_prefix + "." + field_name;
        if (it->value.IsObject()) {
            flattenAndPush(t_db_number, it->value, full_path, t_timestamp_ms);
            {
                std::lock_guard lock(dirty_mutex_);
                dirty_aggregates_.insert(fmt::format("{}:{}", t_db_number, full_path));
            }
            continue;
        }

        const std::string t_map_key = fmt::format("{}:{}", t_db_number, full_path);
        auto node_it = nodes_.find(t_map_key);
        if (node_it == nodes_.end())
            continue;

        const NodeContext* p_ctx = node_it->second.ctx;

        if (p_ctx && p_ctx->trigger_events && alarm_callback_) {
            if (it->value.IsObject()) {
                alarm_callback_(t_db_number, full_path, it->value, t_timestamp_ms);
            } else if (it->value.IsArray()) {
                for (rapidjson::SizeType i = 0; i < it->value.Size(); ++i)
                    alarm_callback_(t_db_number, fmt::format("{}[{}]", full_path, i), it->value[i], t_timestamp_ms);
            } else {
                alarm_callback_(t_db_number, full_path, it->value, t_timestamp_ms);
            }
        }

        UA_DataValue t_raw_dv{};
        bool ok = false;
        if (it->value.IsArray()) {
            const int ua_type_idx =
                p_ctx ? (p_ctx->elem_ua_type_index >= 0 ? p_ctx->elem_ua_type_index : UA_TYPES_DOUBLE) : UA_TYPES_DOUBLE;
            ok = jsonArrayToDataValue(it->value, ua_type_idx, t_raw_dv);
        } else {
            ok = jsonValueToDataValue(it->value, t_raw_dv);
        }
        if (!ok)
            continue;

        enqueueDataValue(node_it->second.node_id, std::move(t_raw_dv), t_timestamp_ms);
        notifyAggregateAncestors(t_db_number, full_path, t_timestamp_ms);
    }
}

void DeltaPushHandler::notifyAggregateAncestors(uint16_t t_db_number, const std::string& t_leaf_path, uint64_t /*timestamp_ms*/) {
    if (t_leaf_path.empty())
        return;
    std::lock_guard lock(dirty_mutex_);
    std::string prefix;
    size_t start = 0;
    while (start < t_leaf_path.size()) {
        size_t dot = t_leaf_path.find('.', start);
        std::string segment = t_leaf_path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        prefix = prefix.empty() ? segment : prefix + "." + segment;
        dirty_aggregates_.insert(fmt::format("{}:{}", t_db_number, prefix));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
}

void DeltaPushHandler::flushDirtyAggregates(uint64_t t_timestamp_ms) {
    std::unordered_set<std::string> to_flush;
    {
        std::lock_guard lock(dirty_mutex_);
        to_flush.swap(dirty_aggregates_);
    }
    for (const auto& key : to_flush) {
        auto colon = key.find(':');
        if (colon == std::string::npos)
            continue;
        uint16_t t_db_number = static_cast<uint16_t>(std::stoi(key.substr(0, colon)));
        std::string path_ = key.substr(colon + 1);
        pushSubtreeSnapshot(t_db_number, path_, t_timestamp_ms);
    }
}

void DeltaPushHandler::pushSubtreeSnapshot(uint16_t t_db_number, const std::string& t_path_prefix, uint64_t t_timestamp_ms) {
    const std::string t_map_key = fmt::format("{}:{}", t_db_number, t_path_prefix);
    auto node_it = nodes_.find(t_map_key);
    if (node_it == nodes_.end())
        return;

    const NodeContext* p_ctx = node_it->second.ctx;
    if (!p_ctx || !p_ctx->server)
        return;

    if (!p_ctx->udt_name.empty() && p_ctx->type_registry) {
        const UA_DataType* p_udt_type = p_ctx->type_registry->find(p_ctx->udt_name);
        const PlcNode* p_node = p_ctx->server->findSymbol(p_ctx->db_number, p_ctx->field_path);
        if (p_udt_type && p_node && p_ctx->server->state()) {
            UA_DataValue t_raw_dv{};
            UA_Variant_init(&t_raw_dv.value);
            if (s7StructToExtensionObjectVariant(*p_node, *p_udt_type, p_ctx->server->state()->tree(), t_raw_dv.value)) {
                t_raw_dv.hasValue = true;
                enqueueDataValue(node_it->second.node_id, std::move(t_raw_dv), t_timestamp_ms);
                return;
            }
            UA_Variant_clear(&t_raw_dv.value);
        }
    }

    auto json_res = p_ctx->server->getSubtreeJson(t_db_number, t_path_prefix);
    if (json_res.hasError())
        return;

    UA_DataValue t_raw_dv{};
    UA_String s = UA_STRING_ALLOC(json_res.value().c_str());
    UA_Variant_setScalarCopy(&t_raw_dv.value, &s, &UA_TYPES[UA_TYPES_STRING]);
    UA_String_clear(&s);
    t_raw_dv.hasValue = true;
    enqueueDataValue(node_it->second.node_id, std::move(t_raw_dv), t_timestamp_ms);
}

} // namespace sgrn::gateway::adapters
