#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/memory_to_ua.hpp>

#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

bool setScalarFromDecoded(const s7codec::DecodedValue& t_dv, const NodeContext* tp_ctx, UA_DataValue& t_data_value) {
    if (t_dv.kind() == s7codec::ValueKind::Bool) {
        UA_Boolean b = t_dv.b();
        UA_Variant_setScalarCopy(&t_data_value.value, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else if (t_dv.kind() == s7codec::ValueKind::SignedInt) {
        if (tp_ctx->type == DataType::Time) {
            UA_Double ms = static_cast<UA_Double>(static_cast<int32_t>(t_dv.i()));
            UA_Variant_setScalarCopy(&t_data_value.value, &ms, &UA_TYPES[UA_TYPES_DOUBLE]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_SBYTE) {
            UA_SByte sb = static_cast<UA_SByte>(t_dv.i());
            UA_Variant_setScalarCopy(&t_data_value.value, &sb, &UA_TYPES[UA_TYPES_SBYTE]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_INT16) {
            UA_Int16 i16 = static_cast<UA_Int16>(t_dv.i());
            UA_Variant_setScalarCopy(&t_data_value.value, &i16, &UA_TYPES[UA_TYPES_INT16]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_INT32) {
            UA_Int32 i32 = static_cast<UA_Int32>(t_dv.i());
            UA_Variant_setScalarCopy(&t_data_value.value, &i32, &UA_TYPES[UA_TYPES_INT32]);
        } else {
            UA_Int64 i64 = t_dv.i();
            UA_Variant_setScalarCopy(&t_data_value.value, &i64, &UA_TYPES[UA_TYPES_INT64]);
        }
    } else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt) {
        if (tp_ctx->type == DataType::Byte) {
            UA_Byte ub = static_cast<UA_Byte>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &ub, &UA_TYPES[UA_TYPES_BYTE]);
        } else if (tp_ctx->type == DataType::Word) {
            UA_UInt16 u16 = static_cast<UA_UInt16>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &u16, &UA_TYPES[UA_TYPES_UINT16]);
        } else if (tp_ctx->type == DataType::DWord) {
            UA_UInt32 u32 = static_cast<UA_UInt32>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &u32, &UA_TYPES[UA_TYPES_UINT32]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_BYTE) {
            UA_Byte ub = static_cast<UA_Byte>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &ub, &UA_TYPES[UA_TYPES_BYTE]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_UINT16) {
            UA_UInt16 u16 = static_cast<UA_UInt16>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &u16, &UA_TYPES[UA_TYPES_UINT16]);
        } else if (tp_ctx->elem_ua_type_index == UA_TYPES_UINT32) {
            UA_UInt32 u32 = static_cast<UA_UInt32>(t_dv.u());
            UA_Variant_setScalarCopy(&t_data_value.value, &u32, &UA_TYPES[UA_TYPES_UINT32]);
        } else {
            UA_UInt64 u64 = t_dv.u();
            UA_Variant_setScalarCopy(&t_data_value.value, &u64, &UA_TYPES[UA_TYPES_UINT64]);
        }
    } else if (t_dv.kind() == s7codec::ValueKind::Float) {
        UA_Float f = t_dv.f();
        UA_Variant_setScalarCopy(&t_data_value.value, &f, &UA_TYPES[UA_TYPES_FLOAT]);
    } else if (t_dv.kind() == s7codec::ValueKind::Double) {
        UA_Double d = t_dv.d();
        UA_Variant_setScalarCopy(&t_data_value.value, &d, &UA_TYPES[UA_TYPES_DOUBLE]);
    } else if (t_dv.kind() == s7codec::ValueKind::String) {
        if (tp_ctx->type == DataType::DTL || tp_ctx->type == DataType::DateTime) {
            // s7codec renders DTL as "YYYY-MM-DD HH:MM:SS.nnnnnnnnn" (see
            // s7shell's dtlToString) — parse it back into a real UA_DateTime
            // instead of shipping the formatted string.
            unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ns = 0;
            if (std::sscanf(t_dv.s().c_str(), "%4u-%2u-%2u %2u:%2u:%2u.%9u", &y, &mo, &d, &h, &mi, &s, &ns) >= 6) {
                UA_DateTimeStruct dts{};
                dts.year = static_cast<UA_Int16>(y);
                dts.month = static_cast<UA_UInt16>(mo);
                dts.day = static_cast<UA_UInt16>(d);
                dts.hour = static_cast<UA_UInt16>(h);
                dts.min = static_cast<UA_UInt16>(mi);
                dts.sec = static_cast<UA_UInt16>(s);
                dts.milliSec = static_cast<UA_UInt16>(ns / 1000000U);
                dts.microSec = static_cast<UA_UInt16>((ns / 1000U) % 1000U);
                dts.nanoSec = static_cast<UA_UInt16>(ns % 1000U);
                UA_DateTime dt = UA_DateTime_fromStruct(dts);
                UA_Variant_setScalarCopy(&t_data_value.value, &dt, &UA_TYPES[UA_TYPES_DATETIME]);
                return true;
            }
            // Malformed DTL string — fall through to string encoding rather than
            // fail the whole read; still better than a mismatched DataType attribute.
        }
        UA_String uas = UA_STRING_ALLOC(t_dv.s().c_str());
        UA_Variant_setScalarCopy(&t_data_value.value, &uas, &UA_TYPES[UA_TYPES_STRING]);
        UA_String_clear(&uas);
    } else {
        return false;
    }
    return true;
}

namespace
{

bool appendDecodedToArray(const s7codec::DecodedValue& t_dv, int t_ua_type_idx, size_t t_index, void* tp_arr) {
    switch (t_ua_type_idx) {
        case UA_TYPES_DOUBLE:
            static_cast<UA_Double*>(tp_arr)[t_index] = t_dv.kind() == s7codec::ValueKind::Double  ? t_dv.d()
                                                       : t_dv.kind() == s7codec::ValueKind::Float ? static_cast<UA_Double>(t_dv.f())
                                                                                                  : static_cast<UA_Double>(t_dv.i());
            return true;
        case UA_TYPES_FLOAT:
            static_cast<UA_Float*>(tp_arr)[t_index] = t_dv.kind() == s7codec::ValueKind::Float ? t_dv.f() : static_cast<UA_Float>(t_dv.d());
            return true;
        case UA_TYPES_INT32:
            static_cast<UA_Int32*>(tp_arr)[t_index] = static_cast<UA_Int32>(t_dv.i());
            return true;
        case UA_TYPES_UINT32:
            static_cast<UA_UInt32*>(tp_arr)[t_index] = static_cast<UA_UInt32>(t_dv.u());
            return true;
        case UA_TYPES_INT64:
            static_cast<UA_Int64*>(tp_arr)[t_index] = t_dv.i();
            return true;
        case UA_TYPES_UINT64:
            static_cast<UA_UInt64*>(tp_arr)[t_index] = t_dv.u();
            return true;
        case UA_TYPES_INT16:
            static_cast<UA_Int16*>(tp_arr)[t_index] = static_cast<UA_Int16>(t_dv.i());
            return true;
        case UA_TYPES_UINT16:
            static_cast<UA_UInt16*>(tp_arr)[t_index] = static_cast<UA_UInt16>(t_dv.u());
            return true;
        case UA_TYPES_BYTE:
            static_cast<UA_Byte*>(tp_arr)[t_index] = static_cast<UA_Byte>(t_dv.u());
            return true;
        case UA_TYPES_SBYTE:
            static_cast<UA_SByte*>(tp_arr)[t_index] = static_cast<UA_SByte>(t_dv.i());
            return true;
        case UA_TYPES_BOOLEAN:
            static_cast<UA_Boolean*>(tp_arr)[t_index] = t_dv.b();
            return true;
        default:
            return false;
    }
}

bool buildTypedArray(const NodeContext* tp_ctx, const uint8_t* tp_raw, size_t t_raw_size, UA_DataValue& t_dv) {
    const size_t n = tp_ctx->array_length;
    const int t_ua_type_idx = tp_ctx->elem_ua_type_index;
    if (n == 0 || t_ua_type_idx < 0)
        return false;

    const UA_DataType* p_ua_type = &UA_TYPES[t_ua_type_idx];
    auto* p_arr = UA_Array_new(n, p_ua_type);
    if (!p_arr)
        return false;

    const int elem_size = s7codec::primitiveSize(tp_ctx->type);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p_elem_ptr = tp_raw;
        int bit_idx = 0;
        if (tp_ctx->type == DataType::Bool) {
            p_elem_ptr = tp_raw + (i / 8);
            bit_idx = static_cast<int>(i % 8);
        } else {
            p_elem_ptr = tp_raw + (i * static_cast<size_t>(elem_size));
        }
        if (p_elem_ptr >= tp_raw + t_raw_size)
            break;

        auto decoded = s7codec::decodeScalar(tp_ctx->type, p_elem_ptr, t_raw_size - static_cast<size_t>(p_elem_ptr - tp_raw), bit_idx);
        if (!decoded.valid() || !appendDecodedToArray(decoded, t_ua_type_idx, i, p_arr)) {
            UA_Array_delete(p_arr, n, p_ua_type);
            return false;
        }
    }

    UA_DataValue_init(&t_dv);
    t_dv.hasValue = true;
    UA_Variant_setArray(&t_dv.value, p_arr, n, p_ua_type);
    t_dv.value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
    t_dv.value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
    t_dv.value.arrayDimensionsSize = 1;
    return true;
}

} // namespace

bool s7BytesToDataValue(const NodeContext* tp_ctx, const uint8_t* tp_raw, size_t t_raw_size, UA_DataValue& t_dv) {
    if (!tp_ctx || !tp_raw || t_raw_size == 0)
        return false;

    if (!tp_ctx->udt_name.empty())
        return false;

    if (tp_ctx->array_length > 0)
        return buildTypedArray(tp_ctx, tp_raw, t_raw_size, t_dv);

    auto decoded = s7codec::decodeScalar(tp_ctx->type, tp_raw, t_raw_size, 0, 1);
    if (!decoded.valid())
        return false;

    UA_DataValue_init(&t_dv);
    t_dv.hasValue = true;
    return setScalarFromDecoded(decoded, tp_ctx, t_dv);
}

} // namespace sgrn::gateway::adapters
