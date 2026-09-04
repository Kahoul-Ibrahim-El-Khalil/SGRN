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

// Defined in OpcUaAdapter.cpp — set to true while the adapter's internal
// pending-write flush is calling server->writeDataValue(), so that open62541's
// DataSource write callback (writeValue below) can detect the re-entrant call
// and skip writing back to PLC memory, breaking the recursive loop.
extern thread_local bool g_is_internal_opcua_write;

// Placeholder: extract client IP from UA session when SessionRegistry hooks are wired up.
// Until installAccessControl is implemented, this returns "" (matching original behavior).
static std::string resolveSessionIp(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

// Placeholder: extract client certificate subject from UA session.
static std::string resolveCertSubject(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

static const UA_DataType* normalizeIncomingType(const NodeContext& ctx, const UA_Variant& value) {
    SGRN_RETURN_IF_NULL(value.type, nullptr);

    SGRN_RETURN_IF(ctx.enum_type && value.type == &UA_TYPES[UA_TYPES_INT32], ctx.enum_type);

    return value.type;
}

/**
 * @brief Resolves the UA_DataType this field's schema actually expects for
 *        a STRUCTURE write.
 *
 * NodeContext::udt_name is only populated when the field is a Struct
 * (see node_registration.cpp: `.udt_name = kind_of(field) == FieldKind::Struct
 * ? field.udt_name : ""`), so an empty name means "not a UDT field — nothing
 * to validate." For UDT fields, look the name up in the same TypeRegistry
 * that was used to register the type in the first place. TypeRegistry::find()
 * is a read-only lookup into a table built once at startup (adopt()) and
 * never swapped while serving, so this is safe to call from the write
 * callback without extra locking — same lifetime assumption NodeContext
 * already makes for its other caches.
 */
static const UA_DataType* resolveExpectedUdtType(const NodeContext& ctx) {
    SGRN_RETURN_IF(ctx.udt_name.empty(), nullptr);

    return ctx.type_registry ? ctx.type_registry->find(ctx.udt_name) : nullptr;
}

/**
 * @brief Encodes an incoming OPC UA value into the flat S7 byte layout for
 *        `p_node`. Pure encode — does not touch PlcMemory or the command
 *        queue. Returns the ready-to-commit buffer (including the 4-byte
 *        dynamic-array header if `p_node->is_dynamic_`).
 */
Result<std::vector<uint8_t>, OpcUaAdapterError> encodeBinaryWrite(NodeContext* p_ctx, const UA_Variant& t_v, const PlcNode* p_node) {
    const UA_DataType* p_udt_type = normalizeIncomingType(*p_ctx, t_v);
    const uint8_t* p_udt_data = nullptr;
    bool is_array_input = (t_v.arrayLength > 0);

    // The type this field is actually registered as. If udt_name is
    // non-empty (this is a struct field) but the lookup fails, that's a
    // registry desync — we treat it the same as a mismatch below rather
    // than silently trusting whatever the client sent.
    const UA_DataType* p_expected_udt = resolveExpectedUdtType(*p_ctx);

    if (is_array_input) {
        if (t_v.arrayLength == p_ctx->array_length || (p_node->is_dynamic_ && t_v.arrayLength <= p_ctx->array_length)) {
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
            p_udt_data = static_cast<const uint8_t*>(t_v.data);
        }
    }

    // Distrust the client: whether the struct arrived wrapped in an
    // ExtensionObject or as a direct scalar of a registered UA_DataType,
    // it can claim to be ANY struct type the server knows about. Reject
    // any mismatch here, before members are ever zipped index-by-index
    // against our PlcNode schema tree — this must run for BOTH paths
    // above, not just the ExtensionObject one.
    SGRN_RETURN_ERROR_IF(
        !p_ctx->udt_name.empty() && p_udt_type && p_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE && p_udt_type != p_expected_udt,
        OpcUaAdapterError::TYPE_MISMATCH);

    SGRN_RETURN_ERROR_IF(!p_udt_type || !p_udt_data, OpcUaAdapterError::NULL_POINTER);

    const size_t element_span = std::max<size_t>(1, p_node->size_);
    const size_t input_count = is_array_input ? std::min(t_v.arrayLength, static_cast<size_t>(p_ctx->array_length)) : 1U;
    const size_t header_offset = p_node->is_dynamic_ ? 4U : 0U;
    const bool is_bool_array =
        is_array_input && p_node->type_ == DataType::Bool && UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_BOOLEAN]);
    const size_t payload_size = is_bool_array ? boolArrayByteCount(input_count) : (element_span * input_count);
    std::vector<uint8_t> cannonical_memory_buf(header_offset + payload_size, 0);

    if (p_node->is_dynamic_)
        s7codec::toEndian<uint32_t>(static_cast<uint32_t>(input_count), cannonical_memory_buf.data(), p_node->endian_);

    if (is_array_input) {
        if (is_bool_array) {

            ArrayOfBoolsEncodingContext ctx{.p_source = reinterpret_cast<const UA_Boolean*>(p_udt_data),
                .count = input_count,
                .p_destination = cannonical_memory_buf.data() + header_offset,
                .destination_size = (size_t)(cannonical_memory_buf.size() - header_offset)};

            SGRN_IF_ERROR_PROPAGATE(encodeArrayOfBoolsToMemory(ctx));
        } else {
            const bool is_eo_arr = UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
            for (size_t j = 0; j < input_count; ++j) {
                uint8_t* p_s7_elem_ptr = cannonical_memory_buf.data() + header_offset + (j * element_span);
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

                SGRN_RETURN_ERROR_IF(!p_ctx->udt_name.empty() && p_current_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE &&
                                         p_current_udt_type != p_expected_udt,
                    OpcUaAdapterError::TYPE_MISMATCH);

                if (p_current_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                    OpcUaEncodingContext elem_ctx{p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, p_node};
                    elem_ctx.depth = 0;
                    SGRN_IF_ERROR_PROPAGATE(encodeStructOpcUaToMemory(elem_ctx));
                } else {
                    PlcScalarView elem_view = makeScalarView(*p_node);
                    elem_view.size = static_cast<uint32_t>(element_span);
                    if (p_node->type_ == DataType::String || p_node->type_ == DataType::WString || p_node->type_ == DataType::XString ||
                        p_node->type_ == DataType::XWString) {
                        if (p_node->type_ == DataType::String || p_node->type_ == DataType::XString) {
                            elem_view.count = (element_span >= 2) ? static_cast<uint32_t>(element_span - 2) : 0;
                        } else {
                            elem_view.count = (element_span >= 4) ? static_cast<uint32_t>((element_span - 4) / 2) : 0;
                        }
                    } else {
                        elem_view.count = 1;
                    }
                    OpcUaEncodingContext elem_ctx{p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, elem_view};
                    SGRN_IF_ERROR_PROPAGATE(encodeScalarOpcUaToMemory(elem_ctx));
                }
            }
        }
    } else if (p_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
        OpcUaEncodingContext elem_ctx{p_udt_type, p_udt_data, cannonical_memory_buf.data() + header_offset, p_node};
        SGRN_IF_ERROR_PROPAGATE(encodeStructOpcUaToMemory(elem_ctx));
    } else if (is_bool_array) {
        const auto* p_bools = static_cast<const UA_Boolean*>(t_v.data);

        ArrayOfBoolsEncodingContext ctx{.p_source = p_bools,
            .count = t_v.arrayLength,
            .p_destination = cannonical_memory_buf.data() + header_offset,
            .destination_size = (size_t)(cannonical_memory_buf.data() - header_offset)};

        SGRN_IF_ERROR_PROPAGATE(encodeArrayOfBoolsToMemory(ctx));

    } else {
        OpcUaEncodingContext elem_ctx{p_udt_type, p_udt_data, cannonical_memory_buf.data() + header_offset, p_node};
        SGRN_IF_ERROR_PROPAGATE(encodeScalarOpcUaToMemory(elem_ctx));
    }

    return cannonical_memory_buf;
}

UA_StatusCode writeValue(UA_Server* tp_ua_server, const UA_NodeId* tp_session_id, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, const UA_NumericRange* /*range*/, const UA_DataValue* tp_data_value) {

    // Guard: this write was initiated by the adapter itself (pushing a PLC event to the
    // OPC-UA address space via server->writeDataValue). open62541 calls the DataSource.write
    // callback even for internal server-initiated writes, so without this guard we would
    // write the value back to PLC memory, re-emit a telemetry event, and loop forever.
    // For temporal types (DTL, LDT) the round-trip is not bit-exact (100ns UA precision
    // vs 1ns PLC precision + timezone offset applied per pass), so memcmp always sees a
    // change and the loop never terminates on its own.
    if (g_is_internal_opcua_write)
        return UA_STATUSCODE_GOOD;

    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);

    SGRN_RETURN_IF(!p_ctx || !p_ctx->server || !p_ctx->security, UA_STATUSCODE_BADINTERNALERROR);

    const std::string client_ip = resolveSessionIp(tp_ua_server, tp_session_id);
    const std::string cert_subject = resolveCertSubject(tp_ua_server, tp_session_id);

    if (!p_ctx->security->authorizeField(
            security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, true, "", {}, cert_subject))
        return UA_STATUSCODE_BADUSERACCESSDENIED;

    SGRN_RETURN_IF(!tp_data_value || !tp_data_value->hasValue, UA_STATUSCODE_BADNODATA);

    const UA_Variant& t_v = tp_data_value->value;
    const PlcNode* p_node = p_ctx->server->findSymbol(p_ctx->db_number, p_ctx->field_path);

    SGRN_RETURN_IF_NULL(p_node, UA_STATUSCODE_BADNODATA);

    auto encoded = encodeBinaryWrite(p_ctx, t_v, p_node);
    SGRN_RETURN_IF(encoded.hasError(), toUAStatusCode(encoded.error()));

    // Commit directly — the same call S7/Modbus/EtherNet/IP/HTTP PUT already
    // make. writeDbMemory() does the version bump (leaf + every ancestor,
    // see the bumpFieldVersions fix below), the SnapshotRegistry patch, and
    // markDirty()+signalDirty() atomically under one lock, so there's no
    // separate queued/batched path left to fall out of sync with it. This
    // also means a genuine commit failure (e.g. RANGE_EXCEEDS_ALLOWED_SPACE)
    // now reaches the client as a real bad status instead of the previous
    // always-GOOD-once-encoded behavior.
    auto write_res = p_ctx->server->writeDbMemory(p_ctx->db_number, p_node->offset_, encoded.value().size(), encoded.value().data());
    SGRN_RETURN_IF(write_res.hasError(), toUAStatusCode(write_res.error()));

    return UA_STATUSCODE_GOOD;
}

} // namespace sgrn::gateway::adapters
