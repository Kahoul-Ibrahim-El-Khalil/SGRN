#include <sgrn/gateway/adapters/ethernetip/CipCodec.hpp>
#include <sgrn/gateway/adapters/ethernetip/TypeTranslation.hpp>
#include <algorithm>
#include <cstring>
#include <s7codec/codec.hpp>
namespace sgrn::gateway::adapters::ethernetip
{

using sgrn::scl::DataType;
using sgrn::scl::DbField;

size_t CipCodec::alignOffset(size_t t_offset, size_t t_alignment) {
    if (t_alignment == 0)
        return t_offset;
    size_t rem = t_offset % t_alignment;
    if (rem == 0)
        return t_offset;
    return t_offset + (t_alignment - rem);
}

size_t CipCodec::computeCipSize(const DbField& t_field) {
    size_t size = 0;
    int limit = std::max(1, t_field.count);

    if (!t_field.children.empty()) {
        size_t struct_size = 0;
        for (const auto& child : t_field.children) {
            size_t align = TypeTranslation::getAlignment(child);
            struct_size = alignOffset(struct_size, align);

            // Re-calculate child size as if it is a single element to find the padded struct footprint
            DbField single_child = child;
            single_child.count = 0;
            struct_size += computeCipSize(single_child);
        }
        size_t struct_align = TypeTranslation::getAlignment(t_field);
        struct_size = alignOffset(struct_size, struct_align);
        size = struct_size * limit;
    } else {
        if (t_field.type == DataType::String || t_field.type == DataType::WString) {
            int capacity = (t_field.struct_size > 0 && t_field.count > 1) ? t_field.struct_size : t_field.count;
            if (capacity <= 0)
                capacity = 254;
            size = (4 + capacity) * limit; // 4 byte len + capacity
        } else if (t_field.type == DataType::XString || t_field.type == DataType::XWString) {
            int capacity = t_field.count > 0 ? t_field.count : 1024;
            size = (4 + capacity) * limit; // 4 byte len + capacity
        } else {
            size_t elem_size = sgrn::scl::rawTypeSpanBytes(t_field.type, 1);
            if (t_field.type == DataType::Bool && limit > 1) {
                // Pack Bools as 1 byte per bool for explicit message arrays to be safe
                size = limit;
            } else {
                size = elem_size * limit;
            }
        }
    }
    return size;
}

void CipCodec::encodeToCip(const DbField& t_field, const uint8_t* tp_s7_buf, uint8_t* tp_cip_buf, size_t& t_cip_offset) {
    int limit = std::max(1, t_field.count);

    if (!t_field.children.empty()) {
        size_t struct_align = TypeTranslation::getAlignment(t_field);
        for (int i = 0; i < limit; i++) {
            t_cip_offset = alignOffset(t_cip_offset, struct_align);
            const uint8_t* p_s7_elem = tp_s7_buf + t_field.offset + (i * t_field.struct_size);

            for (const auto& child : t_field.children) {
                t_cip_offset = alignOffset(t_cip_offset, TypeTranslation::getAlignment(child));
                encodeToCip(child, p_s7_elem, tp_cip_buf, t_cip_offset);
            }
            t_cip_offset = alignOffset(t_cip_offset, struct_align);
        }
    } else {
        size_t align = TypeTranslation::getAlignment(t_field);
        if (t_field.type == DataType::String || t_field.type == DataType::WString) {
            int capacity = (t_field.struct_size > 0 && t_field.count > 1) ? t_field.struct_size : t_field.count;
            if (capacity <= 0)
                capacity = 254;

            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, 4);
                const uint8_t* p_s7_str = tp_s7_buf + t_field.offset + i * (2 + capacity);
                uint8_t cur_len = p_s7_str[1];

                s7codec::toEndian<uint32_t>(cur_len, tp_cip_buf + t_cip_offset, s7codec::Endian::Little);
                t_cip_offset += 4;

                std::memcpy(tp_cip_buf + t_cip_offset, p_s7_str + 2, capacity);
                t_cip_offset += capacity;
            }
        } else if (t_field.type == DataType::XString || t_field.type == DataType::XWString) {
            int capacity = t_field.count > 0 ? t_field.count : 1024;
            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, 4);

                const uint8_t* p_s7_str = tp_s7_buf + t_field.offset + i * (8 + capacity);
                uint32_t cur_len = s7codec::fromEndian<uint32_t>(p_s7_str + 4, t_field.endianness);

                s7codec::toEndian<uint32_t>(cur_len, tp_cip_buf + t_cip_offset, s7codec::Endian::Little);
                t_cip_offset += 4;

                std::memcpy(tp_cip_buf + t_cip_offset, p_s7_str + 8, capacity);
                t_cip_offset += capacity;
            }
        } else {
            size_t elem_size = sgrn::scl::rawTypeSpanBytes(t_field.type, 1);
            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, align);
                const uint8_t* p_s7 = tp_s7_buf + t_field.offset + i * elem_size;
                uint8_t* p_cip = tp_cip_buf + t_cip_offset;

                switch (t_field.type) {
                    case DataType::Word:
                    case DataType::UInt:
                    case DataType::Int: {
                        auto val = s7codec::fromEndian<uint16_t>(p_s7, t_field.endianness);
                        s7codec::toEndian<uint16_t>(val, p_cip, s7codec::Endian::Little);
                        t_cip_offset += 2;
                        break;
                    }
                    case DataType::DWord:
                    case DataType::UDInt:
                    case DataType::DInt:
                    case DataType::Real:
                    case DataType::Time:
                    case DataType::TimeOfDay: {
                        auto val = s7codec::fromEndian<uint32_t>(p_s7, t_field.endianness);
                        s7codec::toEndian<uint32_t>(val, p_cip, s7codec::Endian::Little);
                        t_cip_offset += 4;
                        break;
                    }
                    case DataType::LWord:
                    case DataType::ULInt:
                    case DataType::LInt:
                    case DataType::LReal:
                    case DataType::LTime:
                    case DataType::LTimeOfDay: {
                        auto val = s7codec::fromEndian<uint64_t>(p_s7, t_field.endianness);
                        s7codec::toEndian<uint64_t>(val, p_cip, s7codec::Endian::Little);
                        t_cip_offset += 8;
                        break;
                    }
                    default: {
                        std::memcpy(p_cip, p_s7, elem_size);
                        t_cip_offset += elem_size;
                        break;
                    }
                }
            }
        }
    }
}

void CipCodec::decodeFromCip(const DbField& t_field, const uint8_t* tp_cip_buf, uint8_t* tp_s7_buf, size_t& t_cip_offset) {
    int limit = std::max(1, t_field.count);

    if (!t_field.children.empty()) {
        size_t struct_align = TypeTranslation::getAlignment(t_field);
        for (int i = 0; i < limit; i++) {
            t_cip_offset = alignOffset(t_cip_offset, struct_align);
            uint8_t* p_s7_elem = tp_s7_buf + t_field.offset + (i * t_field.struct_size);

            for (const auto& child : t_field.children) {
                t_cip_offset = alignOffset(t_cip_offset, TypeTranslation::getAlignment(child));
                decodeFromCip(child, tp_cip_buf, p_s7_elem, t_cip_offset);
            }
            t_cip_offset = alignOffset(t_cip_offset, struct_align);
        }
    } else {
        size_t align = TypeTranslation::getAlignment(t_field);

        if (t_field.type == DataType::String || t_field.type == DataType::WString) {
            int capacity = (t_field.struct_size > 0 && t_field.count > 1) ? t_field.struct_size : t_field.count;
            if (capacity <= 0)
                capacity = 254;

            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, 4);

                uint8_t* p_s7_str = tp_s7_buf + t_field.offset + i * (2 + capacity);
                uint32_t cur_len = s7codec::fromEndian<uint32_t>(tp_cip_buf + t_cip_offset, s7codec::Endian::Little);
                t_cip_offset += 4;

                p_s7_str[0] = static_cast<uint8_t>(capacity);
                p_s7_str[1] = static_cast<uint8_t>(std::min<uint32_t>(cur_len, capacity));

                std::memcpy(p_s7_str + 2, tp_cip_buf + t_cip_offset, capacity);
                t_cip_offset += capacity;
            }
        } else if (t_field.type == DataType::XString || t_field.type == DataType::XWString) {
            int capacity = t_field.count > 0 ? t_field.count : 1024;
            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, 4);

                uint8_t* p_s7_str = tp_s7_buf + t_field.offset + i * (8 + capacity);
                uint32_t cur_len = s7codec::fromEndian<uint32_t>(tp_cip_buf + t_cip_offset, s7codec::Endian::Little);
                t_cip_offset += 4;

                s7codec::toEndian<uint32_t>(capacity, p_s7_str, t_field.endianness);
                s7codec::toEndian<uint32_t>(std::min<uint32_t>(cur_len, capacity), p_s7_str + 4, t_field.endianness);

                std::memcpy(p_s7_str + 8, tp_cip_buf + t_cip_offset, capacity);
                t_cip_offset += capacity;
            }
        } else {
            size_t elem_size = sgrn::scl::rawTypeSpanBytes(t_field.type, 1);
            for (int i = 0; i < limit; i++) {
                t_cip_offset = alignOffset(t_cip_offset, align);
                const uint8_t* p_cip = tp_cip_buf + t_cip_offset;
                uint8_t* p_s7 = tp_s7_buf + t_field.offset + i * elem_size;

                switch (t_field.type) {
                    case DataType::Word:
                    case DataType::UInt:
                    case DataType::Int: {
                        auto val = s7codec::fromEndian<uint16_t>(p_cip, s7codec::Endian::Little);
                        s7codec::toEndian<uint16_t>(val, p_s7, t_field.endianness);
                        t_cip_offset += 2;
                        break;
                    }
                    case DataType::DWord:
                    case DataType::UDInt:
                    case DataType::DInt:
                    case DataType::Real:
                    case DataType::Time:
                    case DataType::TimeOfDay: {
                        auto val = s7codec::fromEndian<uint32_t>(p_cip, s7codec::Endian::Little);
                        s7codec::toEndian<uint32_t>(val, p_s7, t_field.endianness);
                        t_cip_offset += 4;
                        break;
                    }
                    case DataType::LWord:
                    case DataType::ULInt:
                    case DataType::LInt:
                    case DataType::LReal:
                    case DataType::LTime:
                    case DataType::LTimeOfDay: {
                        auto val = s7codec::fromEndian<uint64_t>(p_cip, s7codec::Endian::Little);
                        s7codec::toEndian<uint64_t>(val, p_s7, t_field.endianness);
                        t_cip_offset += 8;
                        break;
                    }
                    default: {
                        std::memcpy(p_s7, p_cip, elem_size);
                        t_cip_offset += elem_size;
                        break;
                    }
                }
            }
        }
    }
}

} // namespace sgrn::gateway::adapters::ethernetip
