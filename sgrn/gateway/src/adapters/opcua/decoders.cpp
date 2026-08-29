#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/decoders.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/s7_to_ua.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/utils/strings.hpp>
#include <opcua_codec_table.hpp>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

Result<UA_DataValue, OpcUaAdapterError> decodeScalarToDataValue(const s7codec::DecodedValue& t_dv, const NodeContext& t_ctx) {

    UA_DataValue out;
    UA_DataValue_init(&out);
    out.hasValue = true;

    // Enumerations are projected as 32-bit integers carrying the custom
    // Enum DataType so clients can resolve symbolic names. The underlying S7
    // storage may be a 16-bit (or other width) integer; promote/demote as needed.
    if (t_ctx.enum_type != nullptr) {
        UA_Int32 value = 0;

        if (t_dv.kind() == s7codec::ValueKind::SignedInt)
            value = static_cast<UA_Int32>(t_dv.i());
        else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt)
            value = static_cast<UA_Int32>(t_dv.u());
        else
            return Error(OpcUaAdapterError::ENUM_UNSUPPORTED_KIND);

        UA_Variant_setScalarCopy(&out.value, &value, &UA_TYPES[UA_TYPES_INT32]);

        return out;
    }

    // Temporal scalar types are decoded directly from raw bytes before this
    // point; the codec-table adapter is a "return false" sentinel.
    const bool is_time_or_datetime_type =
        t_ctx.type == DataType::DTL || t_ctx.type == DataType::DateTime || t_ctx.type == DataType::LDT || t_ctx.type == DataType::LDTL;

    SGRN_RETURN_ERROR_IF(is_time_or_datetime_type, OpcUaAdapterError::TYPE_MISMATCH);

    const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(t_ctx.type);

    SGRN_RETURN_IF_NULL(p_entry, OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND);

    SGRN_RETURN_IF(!p_entry->to_ua(t_dv, t_ctx.type, out.value), OpcUaAdapterError::NO_EQUIVALENT_TYPE);

    return out;
}

Result<UA_DataValue, OpcUaAdapterError> decodeTypedArrayToDataValue(const OpcUaDecodingContext& t_ctx) {

    const size_t n = t_ctx.p_node_ctx->array_length;
    const int ua_type_idx = t_ctx.p_node_ctx->elem_ua_type_index;

    SGRN_RETURN_ERROR_IF(n == 0 || ua_type_idx < 0, OpcUaAdapterError::INVALID_ARRAY);

    const UA_DataType* p_ua_type = (t_ctx.p_node_ctx->enum_type != nullptr) ? &UA_TYPES[UA_TYPES_INT32] : &UA_TYPES[ua_type_idx];

    auto* p_arr = UA_Array_new(n, p_ua_type);

    SGRN_RETURN_IF_NULL(p_arr, OpcUaAdapterError::ARRAY_ALLOC_FAILED);

    // Compute per-element stride. For strings field_size is the TOTAL array
    // size, so stride = field_size / n. For non-strings use primitiveSize.
    const bool t_is_string = t_ctx.p_node_ctx->type == DataType::String || t_ctx.p_node_ctx->type == DataType::WString ||
                             t_ctx.p_node_ctx->type == DataType::XString || t_ctx.p_node_ctx->type == DataType::XWString;

    const size_t elem_stride = t_is_string ? (n > 0 ? t_ctx.size / n : 0) : [&]() -> size_t {
        auto opt = s7codec::primitiveSize(t_ctx.p_node_ctx->type);
        return opt ? static_cast<size_t>(*opt) : 0;
    }();

    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p_elem_ptr = t_ctx.p_raw_data;
        int bit_idx = 0;

        if (t_ctx.p_node_ctx->type == DataType::Bool) {
            p_elem_ptr = t_ctx.p_raw_data + (i / 8);
            bit_idx = static_cast<int>(i % 8);
        } else {
            p_elem_ptr = t_ctx.p_raw_data + (i * elem_stride);
        }

        if (p_elem_ptr >= t_ctx.p_raw_data + t_ctx.size)
            break;

        const size_t buf_remaining = t_ctx.size - static_cast<size_t>(p_elem_ptr - t_ctx.p_raw_data);

        const uint32_t decode_count = s7codec::stringDecodeCapacity(t_ctx.p_node_ctx->type, 1, t_ctx.p_node_ctx->string_capacity);

        auto decoded =
            s7codec::decodeScalar(t_ctx.p_node_ctx->type, p_elem_ptr, buf_remaining, bit_idx, decode_count, s7codec::Endian::Big);

        bool appended = false;

        // Temporal arrays are decoded directly into UA_DateTime.
        if (t_ctx.p_node_ctx->type == DataType::DTL || t_ctx.p_node_ctx->type == DataType::DateTime ||
            t_ctx.p_node_ctx->type == DataType::LDT || t_ctx.p_node_ctx->type == DataType::LDTL) {

            auto dt = decodeTemporalBytesToUaDateTime(t_ctx.p_node_ctx->type, p_elem_ptr, buf_remaining, s7codec::Endian::Big);

            if (dt.hasValue()) {
                static_cast<UA_DateTime*>(p_arr)[i] = dt.value();
                appended = true;
            }
        } else if (decoded.valid()) {

            if (p_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
                static_cast<UA_Int32*>(p_arr)[i] =
                    static_cast<UA_Int32>(decoded.kind() == s7codec::ValueKind::SignedInt ? decoded.i() : decoded.u());

                appended = true;
            } else if (t_is_string && decoded.kind() == s7codec::ValueKind::String) {

                auto* p_str_arr = static_cast<UA_String*>(p_arr);
                const std::string& s = decoded.s();

                p_str_arr[i] = UA_String_fromChars(s.c_str());
                appended = true;
            } else {
                const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(t_ctx.p_node_ctx->type);

                if (p_entry) {
                    UA_Variant tmp;
                    UA_Variant_init(&tmp);

                    if (p_entry->to_ua(decoded, t_ctx.p_node_ctx->type, tmp) && tmp.type == p_ua_type) {

                        void* p_target = static_cast<uint8_t*>(p_arr) + (i * p_ua_type->memSize);

                        // Adopt the already-created scalar bytes.
                        std::memcpy(p_target, tmp.data, p_ua_type->memSize);

                        UA_free(tmp.data);
                        appended = true;
                    } else {
                        UA_Variant_clear(&tmp);
                    }
                }
            }
        }

        SGRN_RETURN_ERROR_IF(!appended, OpcUaAdapterError::DECODE_FAILED);
    }

    UA_DataValue out;
    UA_DataValue_init(&out);
    out.hasValue = true;

    UA_Variant_setArray(&out.value, p_arr, n, p_ua_type);

    out.value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));

    SGRN_RETURN_IF_NULL(out.value.arrayDimensions, OpcUaAdapterError::ARRAY_ALLOC_FAILED);

    out.value.arrayDimensions[0] = static_cast<UA_UInt32>(n);

    out.value.arrayDimensionsSize = 1;

    return out;
}

Result<UA_DataValue, OpcUaAdapterError> decodeMemoryBytesToDataValue(const OpcUaDecodingContext& t_ctx) {

    SGRN_RETURN_IF_NULL(t_ctx.p_node_ctx, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(t_ctx.p_raw_data, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_ERROR_IF(t_ctx.size == 0, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_ERROR_IF(!t_ctx.p_node_ctx->udt_name.empty(), OpcUaAdapterError::INVALID_DB_ENTRY);

    if (t_ctx.p_node_ctx->array_length > 0)
        return decodeTypedArrayToDataValue(t_ctx);

    // Temporal scalar types: decode raw bytes directly into UA_DateTime.
    if (t_ctx.p_node_ctx->type == DataType::DTL || t_ctx.p_node_ctx->type == DataType::DateTime ||
        t_ctx.p_node_ctx->type == DataType::LDT || t_ctx.p_node_ctx->type == DataType::LDTL) {

        UA_DateTime ua_dt;

        SGRN_ASSIGN_OR_RETURN(
            ua_dt, decodeTemporalBytesToUaDateTime(t_ctx.p_node_ctx->type, t_ctx.p_raw_data, t_ctx.size, s7codec::Endian::Big));

        UA_DataValue out;
        UA_DataValue_init(&out);
        out.hasValue = true;

        UA_Variant_setScalarCopy(&out.value, &ua_dt, &UA_TYPES[UA_TYPES_DATETIME]);

        return out;
    }

    const uint32_t decode_count = s7codec::stringDecodeCapacity(t_ctx.p_node_ctx->type, 1, t_ctx.p_node_ctx->string_capacity);

    auto decoded = s7codec::decodeScalar(t_ctx.p_node_ctx->type, t_ctx.p_raw_data, t_ctx.size, 0, decode_count);

    SGRN_RETURN_ERROR_IF(!decoded.valid(), OpcUaAdapterError::DECODE_FAILED);

    return decodeScalarToDataValue(decoded, *t_ctx.p_node_ctx);
}

namespace
{

size_t leafS7Span(const PlcScalarView& t_view) {
    if (t_view.type == DataType::String || t_view.type == DataType::WString || t_view.type == DataType::XString ||
        t_view.type == DataType::XWString) {

        return static_cast<size_t>(t_view.size);
    }

    if (t_view.count > 1) {
        auto span_opt = s7codec::typeSpanBytes(t_view.type, static_cast<uint32_t>(t_view.count));
        return span_opt ? static_cast<size_t>(*span_opt) : 0;
    }

    auto prim_opt = s7codec::primitiveSize(t_view.type);
    return prim_opt ? static_cast<size_t>(*prim_opt) : 0;
}

Result<void, OpcUaAdapterError> writeDecodedToUaMember(
    const s7codec::DecodedValue& t_dv, DataType t_type, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {

    SGRN_RETURN_IF_NULL(tp_ua_type, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(tp_ua_ptr, OpcUaAdapterError::NULL_POINTER);

    if (tp_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
        UA_Int32 val = 0;

        if (t_dv.kind() == s7codec::ValueKind::SignedInt) {
            val = static_cast<UA_Int32>(t_dv.i());
        } else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt) {
            val = static_cast<UA_Int32>(t_dv.u());
        } else {
            return Error(OpcUaAdapterError::ENUM_UNSUPPORTED_KIND);
        }

        *reinterpret_cast<UA_Int32*>(tp_ua_ptr) = val;
        return {};
    }

    const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(t_type);

    SGRN_RETURN_IF_NULL(p_entry, OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND);

    UA_Variant tmp;
    UA_Variant_init(&tmp);

    SGRN_RETURN_IF(!p_entry->to_ua(t_dv, t_type, tmp), OpcUaAdapterError::NO_EQUIVALENT_TYPE);

    if (tmp.type != tp_ua_type) {
        UA_Variant_clear(&tmp);
        return Error(OpcUaAdapterError::MEMBER_TYPE_MISMATCH);
    }

    // to_ua() already produced a single, self-contained heap allocation of
    // tp_ua_type (UA_new + UA_copy), including any nested heap buffers
    // (e.g. UA_String.data). Adopt those bytes directly into the member
    // slot instead of UA_copy() followed by UA_Variant_clear().
    std::memcpy(tp_ua_ptr, tmp.data, tp_ua_type->memSize);

    // Release only the outer scalar allocation. Nested buffers are now owned
    // by tp_ua_ptr.
    UA_free(tmp.data);

    return {};
}

Result<void, OpcUaAdapterError> decodeArrayOfBooleansFromMemory(const uint8_t* tp_memory_buf, size_t t_count, UA_Boolean*& t_out_arr) {

    t_out_arr = static_cast<UA_Boolean*>(UA_Array_new(t_count, &UA_TYPES[UA_TYPES_BOOLEAN]));

    SGRN_RETURN_IF_NULL(t_out_arr, OpcUaAdapterError::BOOL_ARRAY_ALLOC_FAILED);

    for (size_t j = 0; j < t_count; ++j) {
        const uint8_t* p_bit_ptr = tp_memory_buf + (j / 8U);

        auto dv = s7codec::decodeScalar(DataType::Bool, p_bit_ptr, 1, static_cast<int>(j % 8U));

        t_out_arr[j] = dv.valid() && dv.b();
    }

    return {};
}

} // namespace

Result<void, OpcUaAdapterError> decodeScalarToUa(
    const uint8_t* tp_memory_buf, const PlcScalarView& t_view, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {

    // Temporal scalar types: decode raw bytes directly into UA_DateTime.
    if (t_view.type == DataType::DTL || t_view.type == DataType::DateTime || t_view.type == DataType::LDT ||
        t_view.type == DataType::LDTL) {

        UA_DateTime dt;

        SGRN_ASSIGN_OR_RETURN(dt, decodeTemporalBytesToUaDateTime(t_view.type, tp_memory_buf, leafS7Span(t_view), t_view.endian));

        *reinterpret_cast<UA_DateTime*>(tp_ua_ptr) = dt;
        return {};
    }

    const size_t span = leafS7Span(t_view);

    const uint32_t decode_count = s7codec::stringDecodeCapacity(t_view.type, t_view.count, t_view.string_capacity);

    auto dv = s7codec::decodeScalar(t_view.type, tp_memory_buf, span, t_view.bit_index, decode_count, t_view.endian);

    SGRN_RETURN_ERROR_IF(!dv.valid(), OpcUaAdapterError::DECODE_FAILED);

    return writeDecodedToUaMember(dv, static_cast<DataType>(t_view.type), tp_ua_type, tp_ua_ptr);
}

Result<void, OpcUaAdapterError> decodeToOpcUa(
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

            SGRN_RETURN_ERROR_IF(t_count == 0, OpcUaAdapterError::INVALID_ARRAY);

            if (child.type_ == DataType::Bool) {
                UA_Boolean* p_bool_arr = nullptr;

                SGRN_IF_ERROR_PROPAGATE(decodeArrayOfBooleansFromMemory(tp_memory_buf + child.offset_, t_count, p_bool_arr));

                *reinterpret_cast<size_t*>(tp_ua_ptr + ua_offset) = t_count;

                *reinterpret_cast<void**>(tp_ua_ptr + ua_offset + sizeof(size_t)) = p_bool_arr;

            } else {
                void* p_arr = UA_Array_new(t_count, m.memberType);

                SGRN_RETURN_IF_NULL(p_arr, OpcUaAdapterError::ARRAY_ALLOC_FAILED);

                const size_t elem_stride = std::max<size_t>(1, child.size_);

                for (size_t j = 0; j < t_count; ++j) {
                    uint8_t* p_ua_elem = static_cast<uint8_t*>(p_arr) + (j * m.memberType->memSize);

                    const uint8_t* p_s7_elem = tp_memory_buf + child.offset_ + (j * elem_stride);

                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {

                        SGRN_IF_ERROR_PROPAGATE(decodeToOpcUa(*m.memberType, p_s7_elem, p_ua_elem, child));

                    } else {
                        // Shallow POD projection instead of deep-copying the
                        // child PlcNode per element.
                        PlcScalarView elem_view = makeScalarView(child);

                        elem_view.count = 1;
                        elem_view.size = static_cast<uint32_t>(elem_stride);
                        elem_view.bit_index = 0;

                        SGRN_IF_ERROR_PROPAGATE(decodeScalarToUa(p_s7_elem, elem_view, m.memberType, p_ua_elem));
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

                SGRN_IF_ERROR_PROPAGATE(decodeToOpcUa(*m.memberType, p_s7_member, p_ua_member, child));

            } else {
                SGRN_IF_ERROR_PROPAGATE(decodeScalarToUa(p_s7_member, makeScalarView(child), m.memberType, p_ua_member));
            }

            ua_offset += m.memberType->memSize;
        }
    }

    return {};
}

namespace
{
struct UaMemoryGuard {
    void* p{nullptr};
    explicit UaMemoryGuard(void* pp)
        : p(pp) {
    }
    ~UaMemoryGuard() {
        if (p)
            UA_free(p);
    }
    void release() {
        p = nullptr;
    }
};

} // namespace
Result<UA_Variant, OpcUaAdapterError> decodeStructObjectToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena) {

    SGRN_RETURN_IF_NULL(t_node.cached_slot_, OpcUaAdapterError::INVALID_DB_ENTRY);

    void* p_buf = UA_calloc(1, t_type.memSize);

    SGRN_RETURN_IF_NULL(p_buf, OpcUaAdapterError::ALLOC_FAILED);

    const uint8_t* p_s7_base = t_arena.data() + t_node.cached_slot_->offset + t_node.offset_;

    // p_buf must be freed on every error path, so it is wrapped in a tiny
    // release guard that keeps the macros below one-liners. Ownership is
    // transferred to the ExtensionObject before the guard leaves scope.

    UaMemoryGuard buf_guard{p_buf};
    SGRN_IF_ERROR_PROPAGATE(decodeToOpcUa(t_type, p_s7_base, static_cast<uint8_t*>(buf_guard.p), t_node));

    // Adopt p_buf directly instead of wrapping it in a stack
    // UA_ExtensionObject and deep-copying through UA_Variant_setScalarCopy.
    auto* p_eo = static_cast<UA_ExtensionObject*>(UA_ExtensionObject_new());

    SGRN_RETURN_IF_NULL(p_eo, OpcUaAdapterError::ALLOC_FAILED);

    p_eo->encoding = UA_EXTENSIONOBJECT_DECODED;
    p_eo->content.decoded.type = &t_type;
    p_eo->content.decoded.data = buf_guard.p;
    buf_guard.release();

    UA_Variant out;
    UA_Variant_init(&out);

    UA_Variant_setScalar(&out, p_eo, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);

    return out;
}

} // namespace sgrn::gateway::adapters
