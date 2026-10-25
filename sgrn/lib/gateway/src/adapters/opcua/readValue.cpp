#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/wrappers/opcua/TypeRegistry.hpp>

#include <fmt/core.h>
#include <algorithm>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <optional>
#include <s7codec/codec.hpp>
#include <stdexcept>
#include <string>
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
    // NOTE on offsets: `field_offset`/`field_size` come from the schema's
    // per-field DbField, where members of UDTs are parent-relative. Only
    // top-level DB fields carry absolute arena offsets. The resolved twin
    // symbol (`plc_node`) always carries absolute coordinates, so prefer it
    // whenever it is available — otherwise nested UDT members (e.g.
    // "station.mode" in the enums simulation) read discarded bytes (their
    // relative offsets accidentally resolve into unrelated memory, while the
    // sibling WRITE path uses findSymbol()'s absolute coordinates and works).
    const twin::PlcNode* p_node = tp_ctx->resolveSymbol();
    const uint32_t off = p_node != nullptr ? p_node->offset_ : tp_ctx->field_offset;
    const uint32_t size = p_node != nullptr ? p_node->size_ : tp_ctx->field_size;
    if (tp_ctx->scratch_buf.size() < size)
        return UA_STATUSCODE_BADINTERNALERROR;

    auto r = tp_ctx->server->readDbMemory(tp_ctx->db_number, off, size, tp_ctx->scratch_buf.data());
    SGRN_RETURN_IF(r.hasError(), toUAStatusCode(r.error()));

    OpcUaDecodingContext raw_ctx{.p_node_ctx = tp_ctx, .p_raw_data = tp_ctx->scratch_buf.data(), .size = size};

    auto decoded = decodeMemoryBytesToDataValue(raw_ctx);
    SGRN_RETURN_IF(decoded.hasError(), toUAStatusCode(decoded.error())); // was hard-coded BADINTERNALERROR

    UA_DataValue_init(tp_data_value);
    *tp_data_value = std::move(decoded).value();
    if (t_source_time_stamp)
        setCurrentTimeStamp(tp_data_value);
    return UA_STATUSCODE_GOOD;
}
static UA_StatusCode readArrayValue(const NodeContext* tp_ctx, UA_DataValue* tp_data_value, UA_Boolean t_source_time_stamp) {
    const twin::PlcNode* p_node = tp_ctx->resolveSymbol();
    // PlcNode::size_ is the PER-ELEMENT byte span (see PlcNode::serialize) —
    // the whole-array span is size_ * count_. Using size_ alone read only the
    // first element's bytes and every element beyond decoded as zero.
    const uint32_t size =
        p_node != nullptr ? static_cast<uint32_t>(p_node->size_) * std::max<uint32_t>(1u, p_node->count_) : tp_ctx->field_size;
    const uint32_t off = p_node != nullptr ? p_node->offset_ : tp_ctx->field_offset;
    if (tp_ctx->scratch_buf.size() < size)
        return UA_STATUSCODE_BADINTERNALERROR;

    auto r = tp_ctx->server->readDbMemory(tp_ctx->db_number, off, size, tp_ctx->scratch_buf.data());
    SGRN_RETURN_IF(r.hasError(), toUAStatusCode(r.error()));

    OpcUaDecodingContext raw_ctx{.p_node_ctx = tp_ctx, .p_raw_data = tp_ctx->scratch_buf.data(), .size = size};

    auto result = decodeMemoryBytesToDataValue(raw_ctx);
    SGRN_RETURN_IF(result.hasError(), toUAStatusCode(result.error()));

    *tp_data_value = std::move(result).value();
    if (t_source_time_stamp) {
        setCurrentTimeStamp(tp_data_value);
    }
    return UA_STATUSCODE_GOOD;
}

// Discrete (TIA-style) tag read: scalar tags and scalar UDT members reuse
// the shared decode path over normalized backing bytes (Bool arrives as one
// byte, 0/1 at bit 0).
static UA_StatusCode readTagValue(const NodeContext* tp_ctx, UA_DataValue* tp_data_value, UA_Boolean t_source_time_stamp) {
    if (!tp_ctx->tag_backing || tp_ctx->field_size == 0)
        return UA_STATUSCODE_BADINTERNALERROR;
    auto r = tp_ctx->tag_backing->read_bytes(tp_ctx->tag_name, tp_ctx->tag_byte_offset, tp_ctx->field_size, tp_ctx->tag_bit_index);
    SGRN_RETURN_IF(r.hasError(), UA_STATUSCODE_BADINTERNALERROR);
    if (r.value().size() < tp_ctx->field_size)
        return UA_STATUSCODE_BADINTERNALERROR;
    OpcUaDecodingContext raw_ctx{.p_node_ctx = tp_ctx, .p_raw_data = r.value().data(), .size = tp_ctx->field_size};
    auto decoded = decodeMemoryBytesToDataValue(raw_ctx);
    SGRN_RETURN_IF(decoded.hasError(), toUAStatusCode(decoded.error()));
    UA_DataValue_init(tp_data_value);
    *tp_data_value = std::move(decoded).value();
    if (t_source_time_stamp)
        setCurrentTimeStamp(tp_data_value);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode readValue(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, UA_Boolean t_source_time_stamp, const UA_NumericRange* /*range*/, UA_DataValue* tp_data_value) {
    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    SGRN_RETURN_IF(!p_ctx || (!p_ctx->server && !p_ctx->is_tag), UA_STATUSCODE_BADINTERNALERROR);

    if (p_ctx->security) {
        std::string client_ip = "";
        std::string session_name = "";
        // Tags live outside DB ACLs: default policy applies (field_path carries
        // the tag name for audit).
        const std::optional<uint16_t> db = p_ctx->is_tag ? std::nullopt : std::optional<uint16_t>(p_ctx->db_number);
        const std::string& path = p_ctx->is_tag ? p_ctx->tag_name : p_ctx->field_path;
        SGRN_RETURN_IF(!p_ctx->security->authorizeField(security::Protocol::OpcUA, client_ip, db, path, false, "", {}, session_name),
            UA_STATUSCODE_BADUSERACCESSDENIED);
    }

    if (p_ctx->is_tag)
        return readTagValue(p_ctx, tp_data_value, t_source_time_stamp);

    if (p_ctx->array_length == 0 && p_ctx->udt_name.empty())
        return readScalarValue(p_ctx, tp_data_value, t_source_time_stamp);

    if (p_ctx->array_length > 0 && p_ctx->udt_name.empty() && (p_ctx->elem_ua_type_index >= 0 || p_ctx->enum_type != nullptr))
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
    SGRN_RETURN_IF(!p_ctx || !p_ctx->server, UA_STATUSCODE_BADINTERNALERROR);

    if (p_ctx->security) {
        std::string client_ip = "";
        std::string session_name = "";
        SGRN_RETURN_IF(!p_ctx->security->authorizeField(
                           security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, false, "", {}, session_name),
            UA_STATUSCODE_BADUSERACCESSDENIED);
    }

    if (!p_ctx->udt_name.empty() && p_ctx->type_registry) {
        const UA_DataType* p_udt_type = p_ctx->type_registry->find(p_ctx->udt_name);
        const PlcNode* p_node = p_ctx->resolveSymbol();
        if (p_udt_type && p_node && p_ctx->server->state()) {
            auto result = decodeStructObjectToExtensionObjectVariant(*p_node, *p_udt_type, p_ctx->server->state()->tree());
            SGRN_RETURN_IF(result.hasError(), toUAStatusCode(result.error()));

            UA_Variant_init(&tp_data_value->value);
            tp_data_value->value = std::move(result).value();
            tp_data_value->hasValue = true;
            if (t_source_time_stamp) {
                setCurrentTimeStamp(tp_data_value);
            }
            return UA_STATUSCODE_GOOD;
        }
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADINTERNALERROR;
}
} // namespace sgrn::gateway::adapters
