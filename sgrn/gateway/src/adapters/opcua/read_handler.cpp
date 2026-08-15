#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/memory_to_ua.hpp>
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
        // Stack buffer for small fields (covers all S7 primitives up to LReal/LInt)
        uint8_t stack_buf[16];
        uint8_t* p_raw = stack_buf;
        std::vector<uint8_t> heap_buf;
        if (p_ctx->field_size > sizeof(stack_buf)) {
            heap_buf.resize(p_ctx->field_size);
            p_raw = heap_buf.data();
        }
        if (auto r = p_ctx->server->readDbMemory(p_ctx->db_number, p_ctx->field_offset, p_ctx->field_size, p_raw); r) {
            auto dv = s7codec::decodeScalar(p_ctx->type, p_raw, p_ctx->field_size, 0, 1);
            if (dv.valid()) {
                UA_DataValue_init(tp_data_value);
                tp_data_value->hasValue = true;
                // Shared scalar decode — same type dispatch as memory_to_ua.cpp
                if (!setScalarFromDecoded(dv, p_ctx, *tp_data_value)) {
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
        uint8_t stack_buf[512];
        uint8_t* p_raw = stack_buf;
        std::vector<uint8_t> heap_buf;
        if (p_ctx->field_size > sizeof(stack_buf)) {
            heap_buf.resize(p_ctx->field_size);
            p_raw = heap_buf.data();
        }
        if (auto r = p_ctx->server->readDbMemory(p_ctx->db_number, p_ctx->field_offset, p_ctx->field_size, p_raw); r) {
            UA_DataValue decoded{};
            if (s7BytesToDataValue(p_ctx, p_raw, p_ctx->field_size, decoded)) {
                UA_DataValue_init(tp_data_value);
                tp_data_value->hasValue = true;
                UA_Variant_copy(&decoded.value, &tp_data_value->value);
                UA_DataValue_clear(&decoded);
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
        if (ua_type_idx == UA_TYPES_DOUBLE) {
            auto* p_arr = static_cast<UA_Double*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_DOUBLE]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber() ? jv[static_cast<rapidjson::SizeType>(i)].GetDouble() : 0.0;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_DOUBLE]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_FLOAT) {
            auto* p_arr = static_cast<UA_Float*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_FLOAT]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber()
                               ? static_cast<UA_Float>(jv[static_cast<rapidjson::SizeType>(i)].GetDouble())
                               : 0.0f;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_FLOAT]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_INT32) {
            auto* p_arr = static_cast<UA_Int32*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT32]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber() ? jv[static_cast<rapidjson::SizeType>(i)].GetInt() : 0;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_INT32]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_UINT32) {
            auto* p_arr = static_cast<UA_UInt32*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT32]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber() ? jv[static_cast<rapidjson::SizeType>(i)].GetUint() : 0u;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_UINT32]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_INT64) {
            auto* p_arr = static_cast<UA_Int64*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT64]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber() ? jv[static_cast<rapidjson::SizeType>(i)].GetInt64() : 0;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_INT64]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_UINT64) {
            auto* p_arr = static_cast<UA_UInt64*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT64]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber() ? jv[static_cast<rapidjson::SizeType>(i)].GetUint64() : 0u;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_UINT64]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_INT16) {
            auto* p_arr = static_cast<UA_Int16*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT16]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber()
                               ? static_cast<UA_Int16>(jv[static_cast<rapidjson::SizeType>(i)].GetInt())
                               : 0;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_INT16]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_UINT16) {
            auto* p_arr = static_cast<UA_UInt16*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT16]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber()
                               ? static_cast<UA_UInt16>(jv[static_cast<rapidjson::SizeType>(i)].GetUint())
                               : 0u;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_UINT16]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_BYTE) {
            auto* p_arr = static_cast<UA_Byte*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_BYTE]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber()
                               ? static_cast<UA_Byte>(jv[static_cast<rapidjson::SizeType>(i)].GetUint())
                               : 0u;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_BYTE]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_SBYTE) {
            auto* p_arr = static_cast<UA_SByte*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_SBYTE]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsNumber()
                               ? static_cast<UA_SByte>(jv[static_cast<rapidjson::SizeType>(i)].GetInt())
                               : 0;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_SBYTE]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_BOOLEAN) {
            auto* p_arr = static_cast<UA_Boolean*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_BOOLEAN]));
            for (size_t i = 0; i < n; ++i)
                p_arr[i] = jv[static_cast<rapidjson::SizeType>(i)].IsBool() ? jv[static_cast<rapidjson::SizeType>(i)].GetBool() : false;
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_BOOLEAN]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_STRING) {
            auto* p_arr = static_cast<UA_String*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_STRING]));
            for (size_t i = 0; i < n; ++i) {
                if (jv[static_cast<rapidjson::SizeType>(i)].IsString()) {
                    p_arr[i] = UA_STRING_ALLOC(jv[static_cast<rapidjson::SizeType>(i)].GetString());
                } else {
                    UA_String_init(&p_arr[i]);
                }
            }
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_STRING]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_BYTESTRING) {
            auto* p_arr = static_cast<UA_ByteString*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_BYTESTRING]));
            for (size_t i = 0; i < n; ++i) {
                if (jv[static_cast<rapidjson::SizeType>(i)].IsString()) {
                    p_arr[i] = UA_BYTESTRING_ALLOC(jv[static_cast<rapidjson::SizeType>(i)].GetString());
                } else {
                    UA_ByteString_init(&p_arr[i]);
                }
            }
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_BYTESTRING]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else if (ua_type_idx == UA_TYPES_DATETIME) {
            // Fix 8: Parse actual Unix epoch milliseconds from JSON instead of using server time
            auto* p_arr = static_cast<UA_DateTime*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_DATETIME]));
            for (size_t i = 0; i < n; ++i) {
                const auto& elem = jv[static_cast<rapidjson::SizeType>(i)];
                if (elem.IsUint64()) {
                    uint64_t epoch_ms = elem.GetUint64();
                    p_arr[i] = UA_DateTime_fromUnixTime(static_cast<UA_Int64>(epoch_ms / 1000)) +
                               static_cast<UA_DateTime>((epoch_ms % 1000) * UA_DATETIME_MSEC);
                } else if (elem.IsInt64()) {
                    int64_t epoch_ms = elem.GetInt64();
                    if (epoch_ms >= 0) {
                        p_arr[i] = UA_DateTime_fromUnixTime(static_cast<UA_Int64>(epoch_ms / 1000)) +
                                   static_cast<UA_DateTime>((static_cast<uint64_t>(epoch_ms) % 1000) * UA_DATETIME_MSEC);
                    } else {
                        p_arr[i] = UA_DateTime_fromUnixTime(0);
                    }
                } else {
                    p_arr[i] = UA_DateTime_fromUnixTime(0);
                }
            }
            UA_Variant_setArray(&tp_data_value->value, p_arr, n, &UA_TYPES[UA_TYPES_DATETIME]);
            tp_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            tp_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
            tp_data_value->value.arrayDimensionsSize = 1;
        } else {
            UA_String s = UA_STRING_ALLOC(val_res.value().c_str());
            UA_Variant_setScalarCopy(&tp_data_value->value, &s, &UA_TYPES[UA_TYPES_STRING]);
            UA_String_clear(&s);
        }
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
