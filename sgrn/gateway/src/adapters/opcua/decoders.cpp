#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>

#include <opcua_codec_table.hpp>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

Result<void, std::string> setScalarFromDecoded(const s7codec::DecodedValue& t_dv, const NodeContext* tp_ctx, UA_DataValue* tp_data_value) {
    // Enumerations are projected as 32-bit integers carrying the custom
    // Enum DataType so clients can resolve symbolic names. The underlying S7
    // storage may be a 16-bit (or other width) integer; promote/demote as needed.
    if (tp_ctx->enum_type != nullptr) {
        UA_Int32 value = 0;
        if (t_dv.kind() == s7codec::ValueKind::SignedInt)
            value = static_cast<UA_Int32>(t_dv.i());
        else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt)
            value = static_cast<UA_Int32>(t_dv.u());
        else
            return Error("Faield to handle Enums");
        UA_Variant_setScalarCopy(&tp_data_value->value, &value, tp_ctx->enum_type);
        return {};
    }

    const sgrn::codecs::CodecEntry* entry = sgrn::codecs::codecEntryFor(tp_ctx->type);
    if (!entry) {
        return Error("Failed to indentify the appropriate Corresponding type");
    }
    if (!entry->to_ua(t_dv, tp_ctx->type, tp_data_value->value)) {
        if (tp_ctx->type == DataType::DTL || tp_ctx->type == DataType::DateTime) {
            if (t_dv.kind() != s7codec::ValueKind::String)
                return Error("Codec failed to render DTL as string");
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
                UA_Variant_setScalarCopy(&tp_data_value->value, &dt, &UA_TYPES[UA_TYPES_DATETIME]);
                return {};
            }
            // Malformed DTL string — fall through to string encoding rather than
            // fail the whole read; still better than a mismatched DataType attribute.
            UA_String uas = UA_STRING_ALLOC(t_dv.s().c_str());
            UA_Variant_setScalarCopy(&tp_data_value->value, &uas, &UA_TYPES[UA_TYPES_STRING]);
            UA_String_clear(&uas);
            return {};
        }
        return {};
    }
    return {};
}

Result<void, std::string> buildTypedArray(RawDecodingContext* tp_ctx) {
    const size_t n = tp_ctx->p_node_ctx->array_length;
    const int t_ua_type_idx = tp_ctx->p_node_ctx->elem_ua_type_index;
    if (n == 0 || t_ua_type_idx < 0) {
        return Error("This is an array of unknown type or of size 0");
    }

    const UA_DataType* p_ua_type = (tp_ctx->p_node_ctx->enum_type != nullptr) ? tp_ctx->p_node_ctx->enum_type : &UA_TYPES[t_ua_type_idx];

    auto* p_arr = UA_Array_new(n, p_ua_type);
    if (!p_arr) {
        return Error("Failed to init Array");
    }

    // Compute per-element stride. For strings field_size is the TOTAL array size,
    // so stride = field_size / n. For non-strings use primitiveSize (handles bool packing
    // separately below).
    const bool t_is_string = (tp_ctx->p_node_ctx->type == DataType::String || tp_ctx->p_node_ctx->type == DataType::WString ||
                              tp_ctx->p_node_ctx->type == DataType::XString || tp_ctx->p_node_ctx->type == DataType::XWString);
    // Stride: for strings derived from total/count; for bools, 0 (handled via bit indexing); for others use primitiveSize.
    const size_t elem_stride =
        t_is_string ? (n > 0 ? (tp_ctx->size / n) : 0) : static_cast<size_t>(s7codec::primitiveSize(tp_ctx->p_node_ctx->type));

    // For strings: char capacity = elem_stride minus the fixed header bytes.

    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p_elem_ptr = tp_ctx->p_raw_data;
        int bit_idx = 0;
        if (tp_ctx->p_node_ctx->type == DataType::Bool) {
            p_elem_ptr = tp_ctx->p_raw_data + (i / 8);
            bit_idx = static_cast<int>(i % 8);
        } else {
            p_elem_ptr = tp_ctx->p_raw_data + (i * elem_stride);
        }
        if (p_elem_ptr >= tp_ctx->p_raw_data + tp_ctx->size)
            break;

        const size_t buf_remaining = tp_ctx->size - static_cast<size_t>(p_elem_ptr - tp_ctx->p_raw_data);
        // For string elements pass char capacity as decode_count; for scalars pass 1.
        const uint32_t decode_count = s7codec::stringDecodeCapacity(tp_ctx->p_node_ctx->type, 1, tp_ctx->p_node_ctx->string_capacity);
        auto decoded = s7codec::decodeScalar(tp_ctx->p_node_ctx->type, p_elem_ptr, buf_remaining, bit_idx, decode_count,
            tp_ctx->p_node_ctx->type == DataType::WString || tp_ctx->p_node_ctx->type == DataType::XWString ? s7codec::Endian::Big
                                                                                                            : s7codec::Endian::Big);

        bool appended = false;
        if (decoded.valid()) {
            if (p_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
                static_cast<UA_Int32*>(p_arr)[i] =
                    static_cast<UA_Int32>(decoded.kind() == s7codec::ValueKind::SignedInt ? decoded.i() : decoded.u());
                appended = true;
            } else if (t_is_string && decoded.kind() == s7codec::ValueKind::String) {
                // String element: allocate UA_String in the array slot
                auto* p_str_arr = static_cast<UA_String*>(p_arr);
                const std::string& s = decoded.s();
                p_str_arr[i] = UA_String_fromChars(s.c_str());
                appended = true;
            } else {
                const sgrn::codecs::CodecEntry* entry = sgrn::codecs::codecEntryFor(tp_ctx->p_node_ctx->type);
                if (entry) {
                    UA_Variant tmp;
                    UA_Variant_init(&tmp);
                    if (entry->to_ua(decoded, tp_ctx->p_node_ctx->type, tmp) && tmp.type == p_ua_type) {
                        void* p_target = static_cast<uint8_t*>(p_arr) + (i * p_ua_type->memSize);
                        appended = (UA_copy(tmp.data, p_target, p_ua_type) == UA_STATUSCODE_GOOD);
                    }
                    UA_Variant_clear(&tmp);
                }
            }
        }

        if (!appended) {
            UA_Array_delete(p_arr, n, p_ua_type);
            return Error("Failed to decode Elements");
        }
    }

    UA_DataValue_init(tp_ctx->p_data_value);
    tp_ctx->p_data_value->hasValue = true;
    UA_Variant_setArray(&(tp_ctx->p_data_value->value), p_arr, n, p_ua_type);
    tp_ctx->p_data_value->value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
    tp_ctx->p_data_value->value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
    tp_ctx->p_data_value->value.arrayDimensionsSize = 1;
    return {};
}

Result<void, std::string> memoryBytesToDataValue(RawDecodingContext* tp_ctx) {
    if (!tp_ctx || !tp_ctx->p_node_ctx || !tp_ctx->p_raw_data || tp_ctx->size == 0) {
        return Error("Null pointer");
    }

    if (!tp_ctx->p_node_ctx->udt_name.empty())
        return Error("Empty udt_name");

    if (tp_ctx->p_node_ctx->array_length > 0)
        return buildTypedArray(tp_ctx);

    const uint32_t decode_count = s7codec::stringDecodeCapacity(tp_ctx->p_node_ctx->type, 1, tp_ctx->p_node_ctx->string_capacity);
    auto decoded = s7codec::decodeScalar(tp_ctx->p_node_ctx->type, tp_ctx->p_raw_data, tp_ctx->size, 0, decode_count);

    if (!decoded.valid()) {
        return "Decoding Scalar Failed";
    }

    UA_DataValue_init(tp_ctx->p_data_value);
    tp_ctx->p_data_value->hasValue = true;
    return setScalarFromDecoded(decoded, tp_ctx->p_node_ctx, tp_ctx->p_data_value);
}

} // namespace sgrn::gateway::adapters
