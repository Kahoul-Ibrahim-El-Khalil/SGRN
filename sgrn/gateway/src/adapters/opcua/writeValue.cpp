/**
 * @file write_handler.cpp
 * @brief Handles inbound OPC UA writes and translates them into S7 PLC memory payloads.
 *
 * This file acts as the primary data ingress point for the OPC UA protocol adapter.
 * When a remote client issues a Write request to the gateway, this file takes the
 * incoming UA_Variant payload, inspects its type, and attempts to map it to the
 * corresponding S7 PLC memory structure (PlcNode).
 *
 * If a native S7 mapping exists, it builds a high-performance binary payload and
 * pushes it directly to the PlcState command queue. If no native mapping exists,
 * it falls back to serializing the data into a generic JSON field.
 */
#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/encoders.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/core.h>
#include <sgrn/gateway/adapters/opcua/encoders.hpp>
#include <opcua_codec_table.hpp>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <vector>
using namespace sgrn::gateway::twin;
using ::sgrn::gateway::S7Area;
using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

// Placeholder: extract client IP from UA session when SessionRegistry hooks are wired up.
// Until installAccessControl is implemented, this returns "" (matching original behavior).
static std::string resolveSessionIp(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

// Placeholder: extract client certificate subject from UA session.
static std::string resolveCertSubject(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

/**
 * @brief Attempts to execute a high-performance native binary write to the PLC.
 *
 * This is the "fast path". It resolves the raw pointers from the OPC UA variant,
 * allocates a correctly sized binary buffer based on the schema (PlcNode), and packs
 * the data using `encodeScalarOpcUaToMemory` or `translateOpcUaToS7`. The resulting binary
 * payload is then wrapped in a PlcCommand and queued for execution by the PlcCommandProcessor.
 *
 * @return UA_STATUSCODE_GOOD on success, an error code on failure, or std::nullopt
 *         if the data type cannot be natively mapped (triggering a JSON fallback).
 */
Result<void, OpcUaAdapterError> tryBinaryWrite(NodeContext* p_ctx, const UA_Variant& t_v, const PlcNode* p_node) {
    const UA_DataType* p_udt_type = nullptr;
    const uint8_t* p_udt_data = nullptr;
    bool is_array_input = (t_v.arrayLength > 0);

    if (is_array_input) {
        if (t_v.arrayLength == p_ctx->array_length || (p_node->is_dynamic_ && t_v.arrayLength <= p_ctx->array_length)) {
            p_udt_type = t_v.type;
            p_udt_data = static_cast<const uint8_t*>(t_v.data);
        }
    } else {
        if (UA_Variant_hasScalarType(&t_v, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT])) {
            const auto* p_eo = static_cast<const UA_ExtensionObject*>(t_v.data);
            if ((p_eo->encoding == UA_EXTENSIONOBJECT_DECODED || p_eo->encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                p_eo->content.decoded.data && p_eo->content.decoded.type) {
                p_udt_type = p_eo->content.decoded.type;
                p_udt_data = static_cast<const uint8_t*>(p_eo->content.decoded.data);
            }
        } else if (t_v.type) {
            p_udt_type = t_v.type;
            p_udt_data = static_cast<const uint8_t*>(t_v.data);
        }
    }

    if (!p_udt_type || !p_udt_data) {
        return OpcUaAdapterError::NULL_POINTER;
    }

    const size_t element_span = std::max<size_t>(1, p_node->size_);
    const size_t input_count = is_array_input ? std::min(t_v.arrayLength, static_cast<size_t>(p_ctx->array_length)) : 1U;
    const size_t header_offset = p_node->is_dynamic_ ? 4U : 0U;
    const bool is_bool_array =
        is_array_input && p_node->type_ == DataType::Bool && UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_BOOLEAN]);
    const size_t payload_size = is_bool_array ? boolArrayByteCount(input_count) : (element_span * input_count);
    std::vector<uint8_t> s7_binary(header_offset + payload_size, 0);

    if (p_node->is_dynamic_)
        s7codec::toEndian<uint32_t>(static_cast<uint32_t>(input_count), s7_binary.data(), p_node->endian_);

    if (is_array_input) {
        if (is_bool_array) {
            const auto* p_bools = reinterpret_cast<const UA_Boolean*>(p_udt_data);
            if (auto result =
                    writeBoolArrayToMemory(p_bools, input_count, s7_binary.data() + header_offset, s7_binary.size() - header_offset);
                result.hasError())
                return result.error();
        } else {
            const bool is_eo_arr = UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
            for (size_t j = 0; j < input_count; ++j) {
                uint8_t* p_s7_elem_ptr = s7_binary.data() + header_offset + (j * element_span);
                const UA_DataType* p_current_udt_type = p_udt_type;
                const uint8_t* p_current_ua_data = p_udt_data + (j * p_udt_type->memSize);

                if (is_eo_arr) {
                    const auto* p_eo_arr = static_cast<const UA_ExtensionObject*>(t_v.data);
                    const UA_ExtensionObject& p_eo = p_eo_arr[j];
                    if ((p_eo.encoding == UA_EXTENSIONOBJECT_DECODED || p_eo.encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                        p_eo.content.decoded.data && p_eo.content.decoded.type) {
                        p_current_udt_type = p_eo.content.decoded.type;
                        p_current_ua_data = static_cast<const uint8_t*>(p_eo.content.decoded.data);
                    }
                }

                if (!p_current_udt_type || !p_current_ua_data)
                    continue;

                if (p_current_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                    if (auto result = encodeStructOpcUaToMemory(*p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, *p_node);
                        result.hasError())
                        return result.error();
                } else {
                    PlcNode elem_node = *p_node;
                    if (p_node->type_ == DataType::String || p_node->type_ == DataType::WString || p_node->type_ == DataType::XString ||
                        p_node->type_ == DataType::XWString) {
                        if (p_node->type_ == DataType::String || p_node->type_ == DataType::XString) {
                            elem_node.count_ = (element_span >= 2) ? static_cast<uint32_t>(element_span - 2) : 0;
                        } else {
                            elem_node.count_ = (element_span >= 4) ? static_cast<uint32_t>((element_span - 4) / 2) : 0;
                        }
                    } else {
                        elem_node.count_ = 1;
                    }
                    elem_node.size_ = static_cast<uint32_t>(element_span);
                    elem_node.offset_ = 0;
                    if (auto result = encodeScalarOpcUaToMemory(p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, elem_node);
                        result.hasError())
                        return result.error();
                }
            }
        }
    } else if (p_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
        if (auto result = encodeStructOpcUaToMemory(*p_udt_type, p_udt_data, s7_binary.data() + header_offset, *p_node); result.hasError())
            return result.error();
    } else if (is_bool_array) {
        const auto* p_bools = static_cast<const UA_Boolean*>(t_v.data);
        if (auto result =
                writeBoolArrayToMemory(p_bools, t_v.arrayLength, s7_binary.data() + header_offset, s7_binary.size() - header_offset);
            result.hasError())
            return result.error();
    } else {
        if (auto result = encodeScalarOpcUaToMemory(p_udt_type, p_udt_data, s7_binary.data() + header_offset, *p_node); result.hasError())
            return result.error();
    }

    PlcCommand cmd;
    cmd.type = PlcCommand::WriteBinary;
    cmd.db_number = p_ctx->db_number;
    cmd.size = s7_binary.size();
    cmd.offset = p_node->offset_ + (is_array_input ? 0 : 0);

    auto* p_entry = p_ctx->server->state()->findSegmentById(p_ctx->db_number);

    cmd.path = (p_entry ? p_entry->name : "") + "." + p_ctx->field_path;

    cmd.value_binary = std::move(s7_binary);

    cmd.timestamp = sgrn::utils::time::nowMilliseconds();

    p_ctx->server->state()->pushCommand(std::move(cmd));

    p_ctx->server->signalDirty();

    return {};
}

/**
 * @brief The main OPC UA write interceptor callback registered with the open62541 server.
 *
 * Orchestrates the entire write pipeline:
 * 1. Validates session security and access rights.
 * 2. Attempts the high-performance native binary write (tryBinaryWrite).
 * 3. Falls back to JSON serialization (tryJsonWrite) if necessary.
 */
UA_StatusCode writeValue(UA_Server* tp_ua_server, const UA_NodeId* tp_session_id, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, const UA_NumericRange* /*range*/, const UA_DataValue* tp_data_value) {

    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    if (!p_ctx || !p_ctx->server || !p_ctx->security) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    const std::string client_ip = resolveSessionIp(tp_ua_server, tp_session_id);
    const std::string cert_subject = resolveCertSubject(tp_ua_server, tp_session_id);

    if (!p_ctx->security->authorizeField(
            security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, true, "", {}, cert_subject))
        return UA_STATUSCODE_BADUSERACCESSDENIED;

    if (!tp_data_value || !tp_data_value->hasValue)
        return UA_STATUSCODE_BADNODATA;

    const UA_Variant& t_v = tp_data_value->value;
    const PlcNode* p_node = p_ctx->server->findSymbol(p_ctx->db_number, p_ctx->field_path);

    if (!p_node) {
        return UA_STATUSCODE_BADNODATA;
    }
    if (auto result = tryBinaryWrite(p_ctx, t_v, p_node); result.hasError()) {
        return toUAStatusCode(result.error());
    }
    return UA_STATUSCODE_GOOD;
}

} // namespace sgrn::gateway::adapters
