#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>
#include <sgrn/gateway/adapters/opcua/s7_to_ua.hpp>
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
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <vector>

using ::sgrn::scl::DataType;
using namespace sgrn::gateway::twin;

namespace sgrn::gateway::adapters
{

UA_StatusCode readS7Value(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, UA_Boolean t_source_time_stamp, const UA_NumericRange* /*range*/, UA_DataValue* tp_data_value) {
    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    if (!p_ctx || !p_ctx->server)
        return UA_STATUSCODE_BADINTERNALERROR;

    if (p_ctx->security) {
        // Placeholder values until SessionRegistry is wired up
        std::string client_ip = "";
        std::string session_name = "";
        if (!p_ctx->security->authorizeField(
                security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, false, "", {}, session_name)) {
            return UA_STATUSCODE_BADUSERACCESSDENIED;
        }
    }

    // Direct raw memory read for scalar primitives
    if (p_ctx->array_length == 0 && p_ctx->udt_name.empty()) {
        if (auto r = p_ctx->server->readDbMemory(p_ctx->db_number, p_ctx->field_offset, p_ctx->field_size, p_ctx->scratch_buf.data()); r) {
            const uint32_t decode_count = s7codec::stringDecodeCapacity(p_ctx->type, 1, p_ctx->string_capacity);

            auto dv = s7codec::decodeScalar(p_ctx->type, p_ctx->scratch_buf.data(), p_ctx->field_size, 0, decode_count);
            if (dv.valid()) {
                UA_DataValue_init(tp_data_value);
                tp_data_value->hasValue = true;
                // Shared scalar decode — same type dispatch as memory_to_ua.cpp
                if (auto result = setScalarFromDecoded(dv, p_ctx, tp_data_value); result.hasError()) {
                    return UA_STATUSCODE_BADTYPEMISMATCH;
                }
                if (t_source_time_stamp) {
                    tp_data_value->sourceTimestamp = UA_DateTime_now();
                    tp_data_value->hasSourceTimestamp = true;
                }
                return UA_STATUSCODE_GOOD;
            }
        }
    }

    // Direct raw memory read for typed arrays
    if (p_ctx->array_length > 0 && p_ctx->udt_name.empty() && p_ctx->elem_ua_type_index >= 0) {
        if (auto r = p_ctx->server->readDbMemory(p_ctx->db_number, p_ctx->field_offset, p_ctx->field_size, p_ctx->scratch_buf.data()); r) {
            UA_DataValue decoded{};

            RawDecodingContext raw_decoding_ctx{
                .p_node_ctx = p_ctx, .p_raw_data = p_ctx->scratch_buf.data(), .size = p_ctx->field_size, .p_data_value = &decoded};

            if (auto result = memoryBytesToDataValue(&raw_decoding_ctx); result.hasValue()) {

                tp_data_value = &decoded;
                if (t_source_time_stamp) {
                    tp_data_value->sourceTimestamp = UA_DateTime_now();
                    tp_data_value->hasSourceTimestamp = true;
                }
                return UA_STATUSCODE_GOOD;
            }
        }
    }

    auto val_res = p_ctx->server->getFieldValue(p_ctx->db_number, p_ctx->field_path);
    if (val_res.hasError()) {
        return UA_STATUSCODE_BADNOTFOUND;
    }

    rapidjson::Document jv;
    if (jv.Parse(val_res.value().c_str()).HasParseError()) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    // ── Array branch ────────────────────────────────────────────────────────
    if (p_ctx->array_length > 0 && jv.IsArray()) {
        const int ua_type_idx = p_ctx->elem_ua_type_index;
        const size_t n = jv.GetArray().Size();

        // UDT/struct array → array of ExtensionObjects
        if (!p_ctx->udt_name.empty() && p_ctx->type_registry) {
            const UA_DataType* p_udt_type = p_ctx->type_registry->find(p_ctx->udt_name);
            if (!p_udt_type) {
                UA_String s = UA_STRING_ALLOC(val_res.value().c_str());
                UA_Variant_setScalarCopy(&tp_data_value->value, &s, &UA_TYPES[UA_TYPES_STRING]);
                UA_String_clear(&s);
                tp_data_value->hasValue = true;
                return UA_STATUSCODE_GOOD;
            }
            auto* p_eo_arr = static_cast<UA_ExtensionObject*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]));
            bool ok = true;
            for (size_t i = 0; i < n; ++i) {
                UA_ExtensionObject_init(&p_eo_arr[i]);
                if (!jv[static_cast<rapidjson::SizeType>(i)].IsObject()) {
                    ok = false;
                    break;
                }
                auto* p_buf = static_cast<uint8_t*>(UA_calloc(1, p_udt_type->memSize));
                if (!p_buf) {
                    ok = false;
                    break;
                }
                if (!::sgrn::gateway::adapters::serializeUdtStructToMemory(jv[static_cast<rapidjson::SizeType>(i)], *p_udt_type, p_buf)) {
                    UA_free(p_buf);
                    ok = false;
                    break;
                }
                p_eo_arr[i].encoding = UA_EXTENSIONOBJECT_DECODED;
                p_eo_arr[i].content.decoded.type = p_udt_type;
                p_eo_arr[i].content.decoded.data = p_buf;
            }
            if (!ok) {
                UA_Array_delete(p_eo_arr, n, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
                return UA_STATUSCODE_BADINTERNALERROR;
            }
            UA_Variant_setArray(&tp_data_value->value, p_eo_arr, n, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
            tp_data_value->hasValue = true;
            if (t_source_time_stamp) {
                tp_data_value->sourceTimestamp = UA_DateTime_now();
                tp_data_value->hasSourceTimestamp = true;
            }
            return UA_STATUSCODE_GOOD;
        }

        // Typed array
        const UA_DataType* p_ua_type = (p_ctx->enum_type != nullptr) ? p_ctx->enum_type : &UA_TYPES[ua_type_idx];

        auto* p_arr = UA_Array_new(n, p_ua_type);
        if (!p_arr)
            return UA_STATUSCODE_BADINTERNALERROR;

        for (size_t i = 0; i < n; ++i) {
            const auto& elem = jv[static_cast<rapidjson::SizeType>(i)];

            if (p_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
                static_cast<UA_Int32*>(p_arr)[i] = elem.IsNumber() ? elem.GetInt() : 0;
            } else if (ua_type_idx == UA_TYPES_DOUBLE) {
                static_cast<UA_Double*>(p_arr)[i] = elem.IsNumber() ? elem.GetDouble() : 0.0;
            } else if (ua_type_idx == UA_TYPES_FLOAT) {
                static_cast<UA_Float*>(p_arr)[i] = elem.IsNumber() ? static_cast<UA_Float>(elem.GetDouble()) : 0.0f;
            } else if (ua_type_idx == UA_TYPES_INT32) {
                static_cast<UA_Int32*>(p_arr)[i] = elem.IsNumber() ? elem.GetInt() : 0;
            } else if (ua_type_idx == UA_TYPES_UINT32) {
                static_cast<UA_UInt32*>(p_arr)[i] = elem.IsNumber() ? elem.GetUint() : 0u;
            } else if (ua_type_idx == UA_TYPES_INT64) {
                static_cast<UA_Int64*>(p_arr)[i] = elem.IsNumber() ? elem.GetInt64() : 0;
            } else if (ua_type_idx == UA_TYPES_UINT64) {
                static_cast<UA_UInt64*>(p_arr)[i] = elem.IsNumber() ? elem.GetUint64() : 0u;
            } else if (ua_type_idx == UA_TYPES_INT16) {
                static_cast<UA_Int16*>(p_arr)[i] = elem.IsNumber() ? static_cast<UA_Int16>(elem.GetInt()) : 0;
            } else if (ua_type_idx == UA_TYPES_UINT16) {
                static_cast<UA_UInt16*>(p_arr)[i] = elem.IsNumber() ? static_cast<UA_UInt16>(elem.GetUint()) : 0u;
            } else if (ua_type_idx == UA_TYPES_BYTE) {
                static_cast<UA_Byte*>(p_arr)[i] = elem.IsNumber() ? static_cast<UA_Byte>(elem.GetUint()) : 0u;
            } else if (ua_type_idx == UA_TYPES_SBYTE) {
                static_cast<UA_SByte*>(p_arr)[i] = elem.IsNumber() ? static_cast<UA_SByte>(elem.GetInt()) : 0;
            } else if (ua_type_idx == UA_TYPES_BOOLEAN) {
                static_cast<UA_Boolean*>(p_arr)[i] = elem.IsBool() ? elem.GetBool() : false;
            } else if (ua_type_idx == UA_TYPES_STRING) {
                static_cast<UA_String*>(p_arr)[i] = elem.IsString() ? UA_STRING_ALLOC(elem.GetString()) : UA_STRING_NULL;
            } else if (ua_type_idx == UA_TYPES_BYTESTRING) {
                static_cast<UA_ByteString*>(p_arr)[i] = elem.IsString() ? UA_BYTESTRING_ALLOC(elem.GetString()) : UA_BYTESTRING_NULL;
            } else if (ua_type_idx == UA_TYPES_DATETIME) {
                if (elem.IsUint64()) {
                    uint64_t epoch_ms = elem.GetUint64();
                    static_cast<UA_DateTime*>(p_arr)[i] = UA_DateTime_fromUnixTime(static_cast<UA_Int64>(epoch_ms / 1000)) +
                                                          static_cast<UA_DateTime>((epoch_ms % 1000) * UA_DATETIME_MSEC);
                } else if (elem.IsInt64() && elem.GetInt64() >= 0) {
                    int64_t epoch_ms = elem.GetInt64();
                    static_cast<UA_DateTime*>(p_arr)[i] =
                        UA_DateTime_fromUnixTime(static_cast<UA_Int64>(epoch_ms / 1000)) +
                        static_cast<UA_DateTime>((static_cast<uint64_t>(epoch_ms) % 1000) * UA_DATETIME_MSEC);
                } else {
                    static_cast<UA_DateTime*>(p_arr)[i] = UA_DateTime_fromUnixTime(0);
                }
            } else {
                // Ignore unknown types, handled by null array element
            }
        }

        UA_Variant_setArray(&tp_data_value->value, p_arr, n, p_ua_type);
        tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
        tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
        tp_data_value->value.arrayDimensionsSize = 1;
        tp_data_value->hasValue = true;
        if (t_source_time_stamp) {
            tp_data_value->sourceTimestamp = UA_DateTime_now();
            tp_data_value->hasSourceTimestamp = true;
        }
        return UA_STATUSCODE_GOOD;
    }

    // ── Scalar UDT branch ────────────────────────────────────────────────────
    if (!p_ctx->udt_name.empty() && p_ctx->array_length == 0 && jv.IsObject()) {
        const UA_DataType* p_udt_type = p_ctx->type_registry ? p_ctx->type_registry->find(p_ctx->udt_name) : nullptr;
        if (p_udt_type) {
            void* p_buf = UA_calloc(1, p_udt_type->memSize);
            if (p_buf) {
                if (::sgrn::gateway::adapters::serializeUdtStructToMemory(jv, *p_udt_type, static_cast<uint8_t*>(p_buf))) {
                    UA_ExtensionObject eo;
                    UA_ExtensionObject_init(&eo);
                    eo.encoding = UA_EXTENSIONOBJECT_DECODED;
                    eo.content.decoded.type = p_udt_type;
                    eo.content.decoded.data = p_buf;
                    UA_Variant_setScalarCopy(&tp_data_value->value, &eo, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
                    UA_ExtensionObject_clear(&eo);
                    tp_data_value->hasValue = true;
                    if (t_source_time_stamp) {
                        tp_data_value->sourceTimestamp = UA_DateTime_now();
                        tp_data_value->hasSourceTimestamp = true;
                    }
                    return UA_STATUSCODE_GOOD;
                }
                UA_free(p_buf);
            }
        }
    }

    // ── Scalar branch ─────────────────────────────────────────────────────────
    if (jv.IsBool()) {
        UA_Boolean b = jv.GetBool();
        UA_Variant_setScalarCopy(&tp_data_value->value, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else if (jv.IsInt()) {
        UA_Int32 i = jv.GetInt();
        UA_Variant_setScalarCopy(&tp_data_value->value, &i, &UA_TYPES[UA_TYPES_INT32]);
    } else if (jv.IsUint()) {
        UA_UInt32 u = jv.GetUint();
        UA_Variant_setScalarCopy(&tp_data_value->value, &u, &UA_TYPES[UA_TYPES_UINT32]);
    } else if (jv.IsInt64()) {
        UA_Int64 i64 = jv.GetInt64();
        UA_Variant_setScalarCopy(&tp_data_value->value, &i64, &UA_TYPES[UA_TYPES_INT64]);
    } else if (jv.IsDouble()) {
        UA_Double d = jv.GetDouble();
        UA_Variant_setScalarCopy(&tp_data_value->value, &d, &UA_TYPES[UA_TYPES_DOUBLE]);
    } else if (jv.IsString()) {
        UA_String s = UA_STRING_ALLOC(jv.GetString());
        UA_Variant_setScalarCopy(&tp_data_value->value, &s, &UA_TYPES[UA_TYPES_STRING]);
        UA_String_clear(&s);
    } else {
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }

    tp_data_value->hasValue = true;
    if (t_source_time_stamp) {
        tp_data_value->sourceTimestamp = UA_DateTime_now();
        tp_data_value->hasSourceTimestamp = true;
    }
    return UA_STATUSCODE_GOOD;
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
            UA_Variant_init(&tp_data_value->value);
            if (s7StructToExtensionObjectVariant(*p_node, *p_udt_type, p_ctx->server->state()->tree(), tp_data_value->value)) {
                tp_data_value->hasValue = true;
                if (t_source_time_stamp) {
                    tp_data_value->sourceTimestamp = UA_DateTime_now();
                    tp_data_value->hasSourceTimestamp = true;
                }
                return UA_STATUSCODE_GOOD;
            }
            UA_Variant_clear(&tp_data_value->value);
        }
    }

    auto json_res = p_ctx->server->getSubtreeJson(p_ctx->db_number, p_ctx->field_path);
    if (json_res.hasError())
        return UA_STATUSCODE_BADNOTFOUND;

    UA_String s = UA_STRING_ALLOC(json_res.value().c_str());
    UA_Variant_setScalarCopy(&tp_data_value->value, &s, &UA_TYPES[UA_TYPES_STRING]);
    UA_String_clear(&s);
    tp_data_value->hasValue = true;
    if (t_source_time_stamp) {
        tp_data_value->sourceTimestamp = UA_DateTime_now();
        tp_data_value->hasSourceTimestamp = true;
    }
    return UA_STATUSCODE_GOOD;
}
} // namespace sgrn::gateway::adapters
