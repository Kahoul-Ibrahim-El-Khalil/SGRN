#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

namespace sgrn::gateway::adapters
{

bool setPrimitiveFromFieldValue(const rapidjson::Value& t_jv, const UA_DataType* tp_type, uint8_t* tp_buf) {
    if (tp_type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
        if (!t_jv.IsBool())
            return false;
        *(UA_Boolean*)tp_buf = t_jv.GetBool();
    } else if (tp_type == &UA_TYPES[UA_TYPES_SBYTE]) {
        if (!t_jv.IsInt())
            return false;
        *(UA_SByte*)tp_buf = static_cast<UA_SByte>(t_jv.GetInt());
    } else if (tp_type == &UA_TYPES[UA_TYPES_BYTE]) {
        if (!t_jv.IsUint())
            return false;
        *(UA_Byte*)tp_buf = static_cast<UA_Byte>(t_jv.GetUint());
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT16]) {
        if (!t_jv.IsInt())
            return false;
        *(UA_Int16*)tp_buf = static_cast<UA_Int16>(t_jv.GetInt());
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT16]) {
        if (!t_jv.IsUint())
            return false;
        *(UA_UInt16*)tp_buf = static_cast<UA_UInt16>(t_jv.GetUint());
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT32]) {
        if (!t_jv.IsInt())
            return false;
        *(UA_Int32*)tp_buf = static_cast<UA_Int32>(t_jv.GetInt());
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT32]) {
        if (!t_jv.IsUint())
            return false;
        *(UA_UInt32*)tp_buf = static_cast<UA_UInt32>(t_jv.GetUint());
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT64]) {
        if (!t_jv.IsInt64())
            return false;
        *(UA_Int64*)tp_buf = static_cast<UA_Int64>(t_jv.GetInt64());
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT64]) {
        if (!t_jv.IsUint64())
            return false;
        *(UA_UInt64*)tp_buf = static_cast<UA_UInt64>(t_jv.GetUint64());
    } else if (tp_type == &UA_TYPES[UA_TYPES_FLOAT]) {
        if (!t_jv.IsNumber())
            return false;
        *(UA_Float*)tp_buf = static_cast<UA_Float>(t_jv.GetDouble());
    } else if (tp_type == &UA_TYPES[UA_TYPES_DOUBLE]) {
        if (!t_jv.IsNumber())
            return false;
        *(UA_Double*)tp_buf = static_cast<UA_Double>(t_jv.GetDouble());
    } else if (tp_type == &UA_TYPES[UA_TYPES_STRING]) {
        if (!t_jv.IsString())
            return false;
        *(UA_String*)tp_buf = UA_STRING_ALLOC(t_jv.GetString());
    } else {
        return false;
    }
    return true;
}

bool getFieldValueFromPrimitive(
    const uint8_t* tp_buf, const UA_DataType* tp_type, rapidjson::Value& t_jv, rapidjson::Document::AllocatorType& t_allocator) {
    if (tp_type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
        t_jv.SetBool(*(const UA_Boolean*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_SBYTE]) {
        t_jv.SetInt(*(const UA_SByte*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_BYTE]) {
        t_jv.SetUint(*(const UA_Byte*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT16]) {
        t_jv.SetInt(*(const UA_Int16*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT16]) {
        t_jv.SetUint(*(const UA_UInt16*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT32]) {
        t_jv.SetInt(*(const UA_Int32*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT32]) {
        t_jv.SetUint(*(const UA_UInt32*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_INT64]) {
        t_jv.SetInt64(*(const UA_Int64*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_UINT64]) {
        t_jv.SetUint64(*(const UA_UInt64*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_FLOAT]) {
        t_jv.SetDouble(*(const UA_Float*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_DOUBLE]) {
        t_jv.SetDouble(*(const UA_Double*)tp_buf);
    } else if (tp_type == &UA_TYPES[UA_TYPES_STRING]) {
        const UA_String* p_s = (const UA_String*)tp_buf;
        t_jv.SetString(reinterpret_cast<const char*>(p_s->data), static_cast<rapidjson::SizeType>(p_s->length), t_allocator);
    } else {
        return false;
    }
    return true;
}

bool serializeUdtStructToMemory(const rapidjson::Value& t_jv, const UA_DataType& t_type, uint8_t* tp_buf) {
    if (!t_jv.IsObject())
        return false;
    size_t offset = 0;
    for (size_t i = 0; i < t_type.membersSize; ++i) {
        const UA_DataTypeMember& m = t_type.members[i];
        offset += m.padding;
#ifdef UA_ENABLE_TYPEDESCRIPTION
        const char* p_name = m.memberName;
#else
        return false;
#endif
        if (!t_jv.HasMember(p_name)) {
            if (m.isArray)
                offset += sizeof(size_t) + sizeof(void*);
            else
                offset += m.memberType->memSize;
            continue;
        }
        const rapidjson::Value& mv = t_jv[p_name];
        if (m.isArray) {
            if (!mv.IsArray())
                return false;
            const size_t n = mv.Size();
            void* p_array_ptr = UA_Array_new(n, m.memberType);
            if (!p_array_ptr)
                return false;
            for (size_t j = 0; j < n; ++j) {
                uint8_t* p_elem_ptr = static_cast<uint8_t*>(p_array_ptr) + (j * m.memberType->memSize);
                if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                    if (!serializeUdtStructToMemory(mv[static_cast<rapidjson::SizeType>(j)], *m.memberType, p_elem_ptr))
                        return false;
                } else {
                    if (!setPrimitiveFromFieldValue(mv[static_cast<rapidjson::SizeType>(j)], m.memberType, p_elem_ptr))
                        return false;
                }
            }
            *reinterpret_cast<size_t*>(tp_buf + offset) = n;
            *reinterpret_cast<void**>(tp_buf + offset + sizeof(size_t)) = p_array_ptr;
            offset += sizeof(size_t) + sizeof(void*);
        } else {
            uint8_t* p_member_ptr = tp_buf + offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                if (!serializeUdtStructToMemory(mv, *m.memberType, p_member_ptr))
                    return false;
            } else {
                if (!setPrimitiveFromFieldValue(mv, m.memberType, p_member_ptr))
                    return false;
            }
            offset += m.memberType->memSize;
        }
    }
    return true;
}

bool deserializeMemoryToUdtJson(
    const uint8_t* tp_buf, const UA_DataType& t_type, rapidjson::Value& t_jv, rapidjson::Document::AllocatorType& t_allocator) {
    t_jv.SetObject();
    size_t offset = 0;
    for (size_t i = 0; i < t_type.membersSize; ++i) {
        const UA_DataTypeMember& m = t_type.members[i];
        offset += m.padding;
#ifdef UA_ENABLE_TYPEDESCRIPTION
        const char* p_name = m.memberName;
#else
        return false;
#endif
        rapidjson::Value mv;
        if (m.isArray) {
            const size_t n = *reinterpret_cast<const size_t*>(tp_buf + offset);
            const void* p_array_ptr = *reinterpret_cast<const void* const*>(tp_buf + offset + sizeof(size_t));
            mv.SetArray();
            for (size_t j = 0; j < n; ++j) {
                const uint8_t* p_elem_ptr = static_cast<const uint8_t*>(p_array_ptr) + (j * m.memberType->memSize);
                rapidjson::Value ev;
                if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                    if (!deserializeMemoryToUdtJson(p_elem_ptr, *m.memberType, ev, t_allocator))
                        return false;
                } else {
                    if (!getFieldValueFromPrimitive(p_elem_ptr, m.memberType, ev, t_allocator))
                        return false;
                }
                mv.PushBack(ev, t_allocator);
            }
            offset += sizeof(size_t) + sizeof(void*);
        } else {
            const uint8_t* p_member_ptr = tp_buf + offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                if (!deserializeMemoryToUdtJson(p_member_ptr, *m.memberType, mv, t_allocator))
                    return false;
            } else {
                if (!getFieldValueFromPrimitive(p_member_ptr, m.memberType, mv, t_allocator))
                    return false;
            }
            offset += m.memberType->memSize;
        }
        t_jv.AddMember(rapidjson::StringRef(p_name), mv, t_allocator);
    }
    return true;
}

} // namespace sgrn::gateway::adapters
