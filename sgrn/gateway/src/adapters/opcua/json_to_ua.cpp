#include <sgrn/gateway/adapters/opcua/json_to_ua.hpp>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

namespace sgrn::gateway::adapters
{

bool jsonValueToDataValue(const rapidjson::Value& t_json_val, UA_DataValue& t_dv) {
    UA_DataValue_init(&t_dv);
    t_dv.hasValue = true;

    if (t_json_val.IsBool()) {
        UA_Boolean b = t_json_val.GetBool();
        UA_Variant_setScalarCopy(&t_dv.value, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else if (t_json_val.IsInt()) {
        UA_Int32 i = t_json_val.GetInt();
        UA_Variant_setScalarCopy(&t_dv.value, &i, &UA_TYPES[UA_TYPES_INT32]);
    } else if (t_json_val.IsUint()) {
        UA_UInt32 u = t_json_val.GetUint();
        UA_Variant_setScalarCopy(&t_dv.value, &u, &UA_TYPES[UA_TYPES_UINT32]);
    } else if (t_json_val.IsInt64()) {
        UA_Int64 i64 = t_json_val.GetInt64();
        UA_Variant_setScalarCopy(&t_dv.value, &i64, &UA_TYPES[UA_TYPES_INT64]);
    } else if (t_json_val.IsUint64()) {
        UA_UInt64 u64 = t_json_val.GetUint64();
        UA_Variant_setScalarCopy(&t_dv.value, &u64, &UA_TYPES[UA_TYPES_UINT64]);
    } else if (t_json_val.IsDouble()) {
        UA_Double d = t_json_val.GetDouble();
        UA_Variant_setScalarCopy(&t_dv.value, &d, &UA_TYPES[UA_TYPES_DOUBLE]);
    } else if (t_json_val.IsString()) {
        UA_String s = UA_STRING_ALLOC(t_json_val.GetString());
        UA_Variant_setScalarCopy(&t_dv.value, &s, &UA_TYPES[UA_TYPES_STRING]);
        UA_String_clear(&s);
    } else {
        return false;
    }
    return true;
}

bool jsonArrayToDataValue(const rapidjson::Value& t_json_arr, int t_ua_type_idx, UA_DataValue& t_dv) {
    UA_DataValue_init(&t_dv);
    t_dv.hasValue = true;

    const size_t n = t_json_arr.GetArray().Size();
    if (n == 0)
        return false;

    auto set_dims = [&](UA_Variant& t_var) {
        t_var.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
        t_var.arrayDimensions[0] = static_cast<UA_UInt32>(n);
        t_var.arrayDimensionsSize = 1;
    };

    if (t_ua_type_idx == UA_TYPES_DOUBLE) {
        auto* p_arr = static_cast<UA_Double*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_DOUBLE]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetDouble()
                           : 0.0;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_DOUBLE]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_FLOAT) {
        auto* p_arr = static_cast<UA_Float*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_FLOAT]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? static_cast<UA_Float>(t_json_arr[static_cast<rapidjson::SizeType>(i)].GetDouble())
                           : 0.0f;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_FLOAT]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_INT32) {
        auto* p_arr = static_cast<UA_Int32*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT32]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] =
                t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber() ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetInt() : 0;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_INT32]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_UINT32) {
        auto* p_arr = static_cast<UA_UInt32*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT32]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] =
                t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber() ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetUint() : 0u;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_UINT32]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_INT64) {
        auto* p_arr = static_cast<UA_Int64*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT64]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] =
                t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber() ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetInt64() : 0;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_INT64]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_UINT64) {
        auto* p_arr = static_cast<UA_UInt64*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT64]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetUint64()
                           : 0u;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_UINT64]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_INT16) {
        auto* p_arr = static_cast<UA_Int16*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_INT16]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? static_cast<UA_Int16>(t_json_arr[static_cast<rapidjson::SizeType>(i)].GetInt())
                           : 0;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_INT16]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_UINT16) {
        auto* p_arr = static_cast<UA_UInt16*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_UINT16]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? static_cast<UA_UInt16>(t_json_arr[static_cast<rapidjson::SizeType>(i)].GetUint())
                           : 0u;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_UINT16]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_BYTE) {
        auto* p_arr = static_cast<UA_Byte*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_BYTE]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? static_cast<UA_Byte>(t_json_arr[static_cast<rapidjson::SizeType>(i)].GetUint())
                           : 0u;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_BYTE]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_SBYTE) {
        auto* p_arr = static_cast<UA_SByte*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_SBYTE]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsNumber()
                           ? static_cast<UA_SByte>(t_json_arr[static_cast<rapidjson::SizeType>(i)].GetInt())
                           : 0;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_SBYTE]);
        set_dims(t_dv.value);
    } else if (t_ua_type_idx == UA_TYPES_BOOLEAN) {
        auto* p_arr = static_cast<UA_Boolean*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_BOOLEAN]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = t_json_arr[static_cast<rapidjson::SizeType>(i)].IsBool() ? t_json_arr[static_cast<rapidjson::SizeType>(i)].GetBool()
                                                                                : false;
        UA_Variant_setArray(&t_dv.value, p_arr, n, &UA_TYPES[UA_TYPES_BOOLEAN]);
        set_dims(t_dv.value);
    } else {
        return false;
    }
    return true;
}

} // namespace sgrn::gateway::adapters
