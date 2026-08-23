#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <fmt/core.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>
#include <vector>

using ::sgrn::scl::DataType;
using namespace sgrn::gateway::twin;

namespace sgrn::gateway::adapters
{

void setCurrentTimeStamp(UA_DataValue* tp_data_value) {
    tp_data_value->sourceTimestamp = UA_DateTime_now();
    tp_data_value->hasSourceTimestamp = true;
}
// Direct raw memory read for scalar primitives. decodeMemoryBytesToDataValue
// covers scalars, temporal types (DTL / DATE_AND_TIME) and typed arrays; on
static UA_StatusCode readScalarValue(const NodeContext* tp_ctx, UA_DataValue* tp_data_value, UA_Boolean t_source_time_stamp) {
    auto r = tp_ctx->server->readDbMemory(tp_ctx->db_number, tp_ctx->field_offset, tp_ctx->field_size, tp_ctx->scratch_buf.data());
    if (r.hasError())
        return toUAStatusCode(r.error());

    RawDecodingContext raw_ctx{.p_node_ctx = tp_ctx, .p_raw_data = tp_ctx->scratch_buf.data(), .size = tp_ctx->field_size};
    if (auto result = decodeMemoryBytesToDataValue(raw_ctx); result.hasValue()) {
        UA_DataValue_init(tp_data_value);
        *tp_data_value = result.value();
        if (t_source_time_stamp)
            setCurrentTimeStamp(tp_data_value);
        return UA_STATUSCODE_GOOD;
    } else {
        return toUAStatusCode(result.error()); // was hard-coded BADINTERNALERROR
    }
}
static UA_StatusCode readArrayValue(const NodeContext* tp_ctx, UA_DataValue* tp_data_value, UA_Boolean t_source_time_stamp) {
    auto r = tp_ctx->server->readDbMemory(tp_ctx->db_number, tp_ctx->field_offset, tp_ctx->field_size, tp_ctx->scratch_buf.data());
    if (r.hasError()) {
        return toUAStatusCode(r.error());
    }
    RawDecodingContext raw_ctx{.p_node_ctx = tp_ctx, .p_raw_data = tp_ctx->scratch_buf.data(), .size = tp_ctx->field_size};

    if (auto result = decodeMemoryBytesToDataValue(raw_ctx); result.hasValue()) {
        *tp_data_value = result.value();
        if (t_source_time_stamp) {
            setCurrentTimeStamp(tp_data_value);
        }
        return UA_STATUSCODE_GOOD;
    } else {
        return toUAStatusCode(result.error());
    }
}

UA_StatusCode readValue(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, UA_Boolean t_source_time_stamp, const UA_NumericRange* /*range*/, UA_DataValue* tp_data_value) {
    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    if (!p_ctx || !p_ctx->server)
        return UA_STATUSCODE_BADINTERNALERROR;

    if (p_ctx->security) {
        std::string client_ip = "";
        std::string session_name = "";
        if (!p_ctx->security->authorizeField(
                security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, false, "", {}, session_name)) {
            return UA_STATUSCODE_BADUSERACCESSDENIED;
        }
    }

    if (p_ctx->array_length == 0 && p_ctx->udt_name.empty())
        return readScalarValue(p_ctx, tp_data_value, t_source_time_stamp);

    if (p_ctx->array_length > 0 && p_ctx->udt_name.empty() && p_ctx->elem_ua_type_index >= 0)
        return readArrayValue(p_ctx, tp_data_value, t_source_time_stamp);

    // Node was registered with the plain read data source but doesn't match
    // either scalar or typed-array shape (e.g. an aggregate/struct node
    // wired to readValue instead of readAggregateValue) — this is a
    // registration mismatch, not a data fault.
    return UA_STATUSCODE_BADNOTIMPLEMENTED;
}
UA_StatusCode readAggregateValue(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/, void* /*sessionContext*/,
    const UA_NodeId* /*nodeId*/, void* tp_node_context, UA_Boolean t_source_time_stamp, const UA_NumericRange* /*range*/,
    UA_DataValue* tp_data_value) {
    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    if (!p_ctx || !p_ctx->server)
        return UA_STATUSCODE_BADINTERNALERROR;

    if (p_ctx->security) {
        std::string client_ip = "";
        std::string session_name = "";
        if (!p_ctx->security->authorizeField(
                security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, false, "", {}, session_name)) {
            return UA_STATUSCODE_BADUSERACCESSDENIED;
        }
    }

    if (!p_ctx->udt_name.empty() && p_ctx->type_registry) {
        const UA_DataType* p_udt_type = p_ctx->type_registry->find(p_ctx->udt_name);
        const PlcNode* p_node = p_ctx->server->findSymbol(p_ctx->db_number, p_ctx->field_path);
        if (p_udt_type && p_node && p_ctx->server->state()) {
            auto result = decodeStructObjectToExtensionObjectVariant(*p_node, *p_udt_type, p_ctx->server->state()->tree());
            if (result.hasValue()) {
                UA_Variant_init(&tp_data_value->value);
                tp_data_value->value = result.value();
                tp_data_value->hasValue = true;
                if (t_source_time_stamp) {
                    setCurrentTimeStamp(tp_data_value);
                }
                return UA_STATUSCODE_GOOD;
            } else {
                UA_Variant_clear(&tp_data_value->value);
                return toUAStatusCode(result.error());
            }
        }
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADINTERNALERROR;
}
} // namespace sgrn::gateway::adapters
