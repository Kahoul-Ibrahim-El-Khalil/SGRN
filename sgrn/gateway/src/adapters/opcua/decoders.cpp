#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>

#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/s7_to_ua.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/utils/strings.hpp>
#include <opcua_codec_table.hpp>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

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

    const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(tp_ctx->type);
    if (!p_entry) {
        return Error("Failed to indentify the appropriate Corresponding type");
    }
    if (!p_entry->to_ua(t_dv, tp_ctx->type, tp_data_value->value)) {
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

namespace
{

size_t leafS7Span(const twin::PlcNode& t_node) {
    if (t_node.type_ == DataType::String || t_node.type_ == DataType::WString || t_node.type_ == DataType::XString ||
        t_node.type_ == DataType::XWString)
        return static_cast<size_t>(t_node.size_);
    if (t_node.count_ > 1)
        return static_cast<size_t>(s7codec::typeSpanBytes(t_node.type_, static_cast<int>(t_node.count_)));
    return static_cast<size_t>(s7codec::primitiveSize(t_node.type_));
}

Result<void, std::string> writeDecodedToUaMember(
    const s7codec::DecodedValue& t_dv, DataType t_type, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {
    if (!tp_ua_type || !tp_ua_ptr) {
        return Error("Null Ptr");
    }

    // ── Enum guard ──────────────────────────────────────────────────────────
    if (tp_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
        UA_Int32 val = 0;
        if (t_dv.kind() == s7codec::ValueKind::SignedInt)
            val = static_cast<UA_Int32>(t_dv.i());
        else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt)
            val = static_cast<UA_Int32>(t_dv.u());
        else {
            return Error("Failed at handling enums ");
        }
        *reinterpret_cast<UA_Int32*>(tp_ua_ptr) = val;
        return {};
    }

    // ── Table-driven dispatch ───────────────────────────────────────────────
    const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(t_type);
    if (!p_entry) {
        return Error("Codec Entry not found");
    }

    UA_Variant tmp;
    UA_Variant_init(&tmp);
    if (!p_entry->to_ua(t_dv, t_type, tmp))

        return Error("Equivalent DataType Not found");

    // If types mismatch (e.g., table produced Double but member expects Int32),
    // fallback to a generic cast if possible, or fail. In practice, the UDT
    // builder ensures the member type matches entry->ua_type_idx.
    if (tmp.type != tp_ua_type) {
        UA_Variant_clear(&tmp);
        return Error("Memmber type does not match the looked up type");
    }

    bool ok = (UA_copy(tmp.data, tp_ua_ptr, tp_ua_type) == UA_STATUSCODE_GOOD);
    UA_Variant_clear(&tmp);
    if (!ok) {
        return Error("Failed at copying the enum value");
    }
    return {};
}

Result<void, std::string> decodeArrayOfBooleansFromMemory(const uint8_t* tp_memory_buf, size_t t_count, UA_Boolean*& t_out_arr) {
    t_out_arr = static_cast<UA_Boolean*>(UA_Array_new(t_count, &UA_TYPES[UA_TYPES_BOOLEAN]));
    if (!t_out_arr)
        return Error("Failed to init array of Booleans");
    for (size_t j = 0; j < t_count; ++j) {
        const uint8_t* p_bit_ptr = tp_memory_buf + (j / 8U);

        auto dv = s7codec::decodeScalar(DataType::Bool, p_bit_ptr, 1, static_cast<int>(j % 8U));
        t_out_arr[j] = dv.valid() && dv.b();
    }
    return {};
}

} // namespace

Result<void, std::string> decodeScalarToUa(
    const uint8_t* tp_memory_buf, const twin::PlcNode& t_node, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {
    const size_t span = leafS7Span(t_node);
    const uint32_t decode_count = s7codec::stringDecodeCapacity(t_node.type_, t_node.count_, t_node.string_capacity_);
    auto dv = s7codec::decodeScalar(t_node.type_, tp_memory_buf, span, t_node.bit_index_, decode_count, t_node.endian_);
    if (!dv.valid()) {
        return Error("Failed at decoding");
    }
    return writeDecodedToUaMember(dv, static_cast<DataType>(t_node.type_), tp_ua_type, tp_ua_ptr);
}

Result<void, std::string> decodeToOpcUa(
    const UA_DataType& t_type, const uint8_t* tp_memory_buf, uint8_t* tp_ua_ptr, const twin::PlcNode& t_node) {
    size_t ua_offset = 0;
    for (size_t i = 0; i < t_type.membersSize; ++i) {
        const UA_DataTypeMember& m = t_type.members[i];
        if (i >= t_node.children_.size())
            break;
        const twin::PlcNode& child = t_node.children_[i];
        ua_offset += m.padding;

        if (m.isArray) {
            const size_t t_count = static_cast<size_t>(child.count_);
            if (t_count == 0)
                return Error("Array of count 0");

            if (child.type_ == DataType::Bool) {
                UA_Boolean* p_bool_arr = nullptr;
                if (auto result = decodeArrayOfBooleansFromMemory(tp_memory_buf + child.offset_, t_count, p_bool_arr); result.hasError()) {
                    return result.error();
                }
                *reinterpret_cast<size_t*>(tp_ua_ptr + ua_offset) = t_count;
                *reinterpret_cast<void**>(tp_ua_ptr + ua_offset + sizeof(size_t)) = p_bool_arr;
            } else {
                void* p_arr = UA_Array_new(t_count, m.memberType);
                if (!p_arr)
                    return Error("Failed to create a new array");
                const size_t elem_stride = std::max<size_t>(1, child.size_);
                for (size_t j = 0; j < t_count; ++j) {
                    uint8_t* p_ua_elem = static_cast<uint8_t*>(p_arr) + (j * m.memberType->memSize);
                    const uint8_t* p_s7_elem = tp_memory_buf + child.offset_ + (j * elem_stride);
                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                        if (auto result = decodeToOpcUa(*m.memberType, p_s7_elem, p_ua_elem, child); result.hasError())
                            return result.error();
                    } else {
                        twin::PlcNode elem_node = child;
                        elem_node.count_ = 1;
                        elem_node.size_ = static_cast<uint32_t>(elem_stride);
                        elem_node.bit_index_ = 0;
                        if (auto result = decodeScalarToUa(p_s7_elem, elem_node, m.memberType, p_ua_elem); result.hasError())
                            return result.error();
                    }
                }
                *reinterpret_cast<size_t*>(tp_ua_ptr + ua_offset) = t_count;
                *reinterpret_cast<void**>(tp_ua_ptr + ua_offset + sizeof(size_t)) = p_arr;
            }
            ua_offset += sizeof(size_t) + sizeof(void*);
        } else {
            const uint8_t* p_s7_member = tp_memory_buf + child.offset_;
            uint8_t* p_ua_member = tp_ua_ptr + ua_offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                if (auto result = decodeToOpcUa(*m.memberType, p_s7_member, p_ua_member, child); result.hasError())
                    return result.error();
            } else if (auto result = decodeScalarToUa(p_s7_member, child, m.memberType, p_ua_member); result.hasError()) {
                return result.error();
            }
            ua_offset += m.memberType->memSize;
        }
    }
    return {};
}

Result<void, std::string> decodeStructObjectToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena, UA_Variant& t_out) {
    if (!t_node.cached_slot_)
        return Error("Invalid Db Entry");

    void* p_buf = UA_calloc(1, t_type.memSize);
    if (!p_buf)
        return Error("Allocation of buffer failed");

    const uint8_t* p_s7_base = t_arena.data() + t_node.cached_slot_->offset + t_node.offset_;
    if (auto result = decodeToOpcUa(t_type, p_s7_base, static_cast<uint8_t*>(p_buf), t_node); result.hasError()) {
        UA_free(p_buf);
        return result.error();
    }

    UA_ExtensionObject eo;
    UA_ExtensionObject_init(&eo);
    eo.encoding = UA_EXTENSIONOBJECT_DECODED;
    eo.content.decoded.type = &t_type;
    eo.content.decoded.data = p_buf;
    UA_Variant_setScalarCopy(&t_out, &eo, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
    UA_ExtensionObject_clear(&eo);
    return {};
}
} // namespace sgrn::gateway::adapters
