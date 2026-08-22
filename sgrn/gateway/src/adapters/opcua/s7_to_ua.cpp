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

bool writeDecodedToUaMember(const s7codec::DecodedValue& t_dv, DataType t_type, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {
    if (!tp_ua_type || !tp_ua_ptr)
        return false;

    // ── Enum guard ──────────────────────────────────────────────────────────
    if (tp_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
        UA_Int32 val = 0;
        if (t_dv.kind() == s7codec::ValueKind::SignedInt)
            val = static_cast<UA_Int32>(t_dv.i());
        else if (t_dv.kind() == s7codec::ValueKind::UnsignedInt)
            val = static_cast<UA_Int32>(t_dv.u());
        else
            return false;
        *reinterpret_cast<UA_Int32*>(tp_ua_ptr) = val;
        return true;
    }

    // ── Table-driven dispatch ───────────────────────────────────────────────
    const sgrn::codecs::CodecEntry* entry = sgrn::codecs::codecEntryFor(t_type);
    if (!entry)
        return false;

    UA_Variant tmp;
    UA_Variant_init(&tmp);
    if (!entry->to_ua(t_dv, t_type, tmp))
        return false;

    // If types mismatch (e.g., table produced Double but member expects Int32),
    // fallback to a generic cast if possible, or fail. In practice, the UDT
    // builder ensures the member type matches entry->ua_type_idx.
    if (tmp.type != tp_ua_type) {
        UA_Variant_clear(&tmp);
        return false;
    }

    bool ok = (UA_copy(tmp.data, tp_ua_ptr, tp_ua_type) == UA_STATUSCODE_GOOD);
    UA_Variant_clear(&tmp);
    return ok;
}

bool readBoolArrayFromS7(const uint8_t* tp_s7_ptr, size_t t_count, UA_Boolean*& t_out_arr) {
    t_out_arr = static_cast<UA_Boolean*>(UA_Array_new(t_count, &UA_TYPES[UA_TYPES_BOOLEAN]));
    if (!t_out_arr)
        return false;
    for (size_t j = 0; j < t_count; ++j) {
        const uint8_t* p_bit_ptr = tp_s7_ptr + (j / 8U);
        auto t_dv = s7codec::decodeScalar(DataType::Bool, p_bit_ptr, 1, static_cast<int>(j % 8U));
        t_out_arr[j] = t_dv.valid() && t_dv.b();
    }
    return true;
}

} // namespace

bool decodeScalarS7ToUa(const uint8_t* tp_s7_ptr, const twin::PlcNode& t_node, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr) {
    const size_t span = leafS7Span(t_node);
    auto t_dv = s7codec::decodeScalar(t_node.type_, tp_s7_ptr, span, t_node.bit_index_, t_node.count_, t_node.endian_);
    if (!t_dv.valid())
        return false;
    return writeDecodedToUaMember(t_dv, static_cast<DataType>(t_node.type_), tp_ua_type, tp_ua_ptr);
}

bool translateS7ToOpcUa(const UA_DataType& t_type, const uint8_t* tp_s7_ptr, uint8_t* tp_ua_ptr, const twin::PlcNode& t_node) {
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
                return false;

            if (child.type_ == DataType::Bool) {
                UA_Boolean* p_bool_arr = nullptr;
                if (!readBoolArrayFromS7(tp_s7_ptr + child.offset_, t_count, p_bool_arr))
                    return false;
                *reinterpret_cast<size_t*>(tp_ua_ptr + ua_offset) = t_count;
                *reinterpret_cast<void**>(tp_ua_ptr + ua_offset + sizeof(size_t)) = p_bool_arr;
            } else {
                void* p_arr = UA_Array_new(t_count, m.memberType);
                if (!p_arr)
                    return false;
                const size_t elem_stride = std::max<size_t>(1, child.size_);
                for (size_t j = 0; j < t_count; ++j) {
                    uint8_t* p_ua_elem = static_cast<uint8_t*>(p_arr) + (j * m.memberType->memSize);
                    const uint8_t* p_s7_elem = tp_s7_ptr + child.offset_ + (j * elem_stride);
                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                        if (!translateS7ToOpcUa(*m.memberType, p_s7_elem, p_ua_elem, child))
                            return false;
                    } else {
                        twin::PlcNode elem_node = child;
                        elem_node.count_ = 1;
                        elem_node.size_ = static_cast<uint32_t>(elem_stride);
                        elem_node.bit_index_ = 0;
                        if (!decodeScalarS7ToUa(p_s7_elem, elem_node, m.memberType, p_ua_elem))
                            return false;
                    }
                }
                *reinterpret_cast<size_t*>(tp_ua_ptr + ua_offset) = t_count;
                *reinterpret_cast<void**>(tp_ua_ptr + ua_offset + sizeof(size_t)) = p_arr;
            }
            ua_offset += sizeof(size_t) + sizeof(void*);
        } else {
            const uint8_t* p_s7_member = tp_s7_ptr + child.offset_;
            uint8_t* p_ua_member = tp_ua_ptr + ua_offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                if (!translateS7ToOpcUa(*m.memberType, p_s7_member, p_ua_member, child))
                    return false;
            } else if (!decodeScalarS7ToUa(p_s7_member, child, m.memberType, p_ua_member)) {
                return false;
            }
            ua_offset += m.memberType->memSize;
        }
    }
    return true;
}

bool s7StructToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena, UA_Variant& t_out) {
    if (!t_node.cached_slot_)
        return false;

    void* p_buf = UA_calloc(1, t_type.memSize);
    if (!p_buf)
        return false;

    const uint8_t* p_s7_base = t_arena.data() + t_node.cached_slot_->offset + t_node.offset_;
    if (!translateS7ToOpcUa(t_type, p_s7_base, static_cast<uint8_t*>(p_buf), t_node)) {
        UA_free(p_buf);
        return false;
    }

    UA_ExtensionObject eo;
    UA_ExtensionObject_init(&eo);
    eo.encoding = UA_EXTENSIONOBJECT_DECODED;
    eo.content.decoded.type = &t_type;
    eo.content.decoded.data = p_buf;
    UA_Variant_setScalarCopy(&t_out, &eo, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
    UA_ExtensionObject_clear(&eo);
    return true;
}

} // namespace sgrn::gateway::adapters
