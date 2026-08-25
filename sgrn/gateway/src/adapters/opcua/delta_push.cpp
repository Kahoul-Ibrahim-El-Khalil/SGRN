#include <sgrn/gateway/adapters/opcua/decoders.hpp>
#include <sgrn/gateway/adapters/opcua/delta_push.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <sgrn/Result.hpp>

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
    running_.store(t_running, std::memory_order_release);
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

    if (!t_event.typed_leaf.bytes || !t_event.typed_leaf.meta.valid || !tp_ctx) {
        return false;
    }

    NodeContext typed_ctx = *tp_ctx;
    typed_ctx.type = static_cast<::sgrn::scl::DataType>(t_event.typed_leaf.meta.type);
    typed_ctx.field_size = t_event.typed_leaf.meta.field_size;
    typed_ctx.array_length = t_event.typed_leaf.meta.array_length;
    typed_ctx.elem_ua_type_index = t_event.typed_leaf.meta.elem_ua_type_index;

    OpcUaDecodingContext context{
        .p_node_ctx = &typed_ctx, .p_raw_data = t_event.typed_leaf.bytes->data(), .size = t_event.typed_leaf.bytes->size()};

    auto result = decodeMemoryBytesToDataValue(context);
    SGRN_RETURN_IF(result.hasError(), false);

    t_raw_dv = std::move(result).value();
    return true;
}

void DeltaPushHandler::onTelemetryEvent(const core::TelemetryEvent& t_event) {

    if (!running_.load(std::memory_order_acquire))
        return;

    if (t_event.type != core::EventType::LeafUpdate)
        return;

    const std::string map_key = fmt::format("{}:{}", t_event.db, t_event.path);

    auto node_it = nodes_.find(map_key);
    if (node_it == nodes_.end())
        return;

    const NodeContext* p_node_ctx = node_it->second.ctx;

    UA_DataValue raw_dv{};
    if (!buildDataValueFromEvent(t_event, p_node_ctx, raw_dv))
        return;

    enqueueDataValue(node_it->second.node_id, std::move(raw_dv), t_event.timestamp);

    notifyAggregateAncestors(t_event.db, t_event.path, t_event.timestamp);
}

void DeltaPushHandler::notifyAggregateAncestors(uint16_t t_db_number, const std::string& t_leaf_path, uint64_t /*timestamp_ms*/) {

    if (t_leaf_path.empty())
        return;

    std::lock_guard lock(dirty_mutex_);

    std::string prefix;
    size_t start = 0;

    // Only PROPER ancestors of the leaf carry an aggregate ".Value" node.
    // Marking the leaf itself would make pushSubtreeSnapshot() fall into the
    // JSON-string fallback and write a String variant into a strictly typed
    // variable node, which open62541 rejects with BADTYPEMISMATCH.
    while (start < t_leaf_path.size()) {
        const size_t dot = t_leaf_path.find('.', start);

        if (dot == std::string::npos)
            break;

        const std::string segment(t_leaf_path.substr(start, dot - start));

        prefix = prefix.empty() ? segment : prefix + "." + segment;

        dirty_aggregates_.insert(fmt::format("{}:{}", t_db_number, prefix));

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
        const size_t colon = key.find(':');

        if (colon == std::string::npos)
            continue;

        const uint16_t db_number = static_cast<uint16_t>(std::stoi(key.substr(0, colon)));

        const std::string path = key.substr(colon + 1);

        pushSubtreeSnapshot(db_number, path, t_timestamp_ms);
    }
}

void DeltaPushHandler::pushSubtreeSnapshot(uint16_t t_db_number, const std::string& t_path_prefix, uint64_t t_timestamp_ms) {

    const std::string map_key = fmt::format("{}:{}", t_db_number, t_path_prefix);

    auto node_it = nodes_.find(map_key);
    if (node_it == nodes_.end())
        return;

    const NodeContext* p_ctx = node_it->second.ctx;

    if (!p_ctx || !p_ctx->server)
        return;

    // Prefer the native OPC UA UDT representation when possible.
    if (!p_ctx->udt_name.empty() && p_ctx->type_registry) {
        const UA_DataType* p_udt_type = p_ctx->type_registry->find(p_ctx->udt_name);

        const PlcNode* p_node = p_ctx->resolveSymbol();

        if (p_udt_type && p_node && p_ctx->server->state()) {

            auto result = decodeStructObjectToExtensionObjectVariant(*p_node, *p_udt_type, p_ctx->server->state()->tree());

            if (result.hasValue()) {
                UA_DataValue raw_dv{};
                UA_DataValue_init(&raw_dv);

                raw_dv.value = std::move(result).value();
                raw_dv.hasValue = true;

                enqueueDataValue(node_it->second.node_id, std::move(raw_dv), t_timestamp_ms);

                return;
            }
        }
    }

    // Fallback: publish the subtree snapshot as JSON.
    auto json_res = p_ctx->server->getSubtreeJson(t_db_number, t_path_prefix);

    if (json_res.hasError())
        return;

    UA_DataValue raw_dv{};
    UA_DataValue_init(&raw_dv);

    UA_String s = UA_STRING_ALLOC(json_res.value().c_str());

    UA_Variant_setScalarCopy(&raw_dv.value, &s, &UA_TYPES[UA_TYPES_STRING]);

    UA_String_clear(&s);

    raw_dv.hasValue = true;

    enqueueDataValue(node_it->second.node_id, std::move(raw_dv), t_timestamp_ms);
}

} // namespace sgrn::gateway::adapters
