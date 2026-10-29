// PlcState_serialize.hpp — PlcNode::serialize() definition (internal).
// ─────────────────────────────────────────────────────────────────────────────
// The recursive RapidJSON projection of a PlcNode tree over the flat arena
// lives here rather than in the public PlcState.hpp: it is implementation
// detail, included only by twin .cpp files that drive a writer
// (PlcState.cpp, PlcMemory_query.cpp). Not installed / not staged.
#pragma once

#include <sgrn/gateway/twin/PlcState.hpp>

namespace sgrn::gateway::twin
{

/**
 * @brief Recursive serialization using RapidJSON Writer.
 *
 * SEMANTIC NESTING & SUBTREE EXTRACTION
 * This method provides "First-Class Nesting" by recursively projecting
 * flat arena bytes into a structured JSON tree.
 *   - Depth over Breadth: Handles deeply nested UDTs/Structs natively.
 *   - Granular Access: Allows extracting any sub-path (e.g. Mixer.Pump.Motor)
 *     without serializing the entire memory block.
 */
template <typename Writer>
void PlcNode::serialize(Writer& t_writer, const ::sgrn::ArenaTree& t_arena, int t_depth, size_t t_extra_offset) const {
    if (!cached_slot_) {
        t_writer.Null();
        return;
    }

    // The absolute offset of this node (or the first element if array)
    // is its rel_offset + any offset inherited from parents (extra_offset).
    size_t base_abs_offset = t_extra_offset + offset_;

    int local_count = static_cast<int>(count_);
    if (local_count < 1)
        local_count = 1;

    if (is_dynamic_) {
        // Read 4-byte current count header
        const uint8_t* p_head_ptr = t_arena.data() + cached_slot_->offset + base_abs_offset;
        int current_count = static_cast<int>(s7codec::fromEndian<uint32_t>(p_head_ptr, endian_));
        local_count = std::min(local_count, current_count);
        base_abs_offset += 4; // Advance past the header
    }

    bool is_s7_string = (type_ == s7codec::Type::String || type_ == s7codec::Type::WString || type_ == s7codec::Type::XString ||
                         type_ == s7codec::Type::XWString);

    // After the offset-tracker fix:
    //   - scalar string:  count_=1,  size_=per-element byte span, string_capacity_=chars
    //   - string array:   count_=N,  size_=per-element byte span, string_capacity_=chars
    //   - other arrays:   count_=N,  size_=per-element byte span
    const bool is_string_array = (is_s7_string && local_count > 1);
    bool is_array = (local_count > 1 && (!is_s7_string || is_string_array));

    size_t elem_size = this->size_;
    // For non-array strings, the entire 'size' is the element size.
    // For arrays, 'size' should be the element size.
    // In loadRegistry, n.size for strings is typeSpanBytes(char_capacity), which is the per-element span.
    if (is_array)
        t_writer.StartArray();

    for (int i = 0; i < (is_array ? local_count : 1); ++i) {
        // Offset for the i-th element of the array
        size_t current_abs_offset = base_abs_offset;
        int b_idx = static_cast<int>(bit_index_);

        if (is_array) {
            if (type_ != s7codec::Type::Bool) {
                current_abs_offset += (i * elem_size);
            }
        }

        // Bounds check against arena size to prevent Segfaults
        size_t check_size = children_.empty() ? (type_ == s7codec::Type::Bool ? ((is_array ? (local_count + 7) / 8 : 1)) : elem_size) : 0;
        if (current_abs_offset + check_size > cached_slot_->offset + cached_slot_->size) {
            if (children_.empty())
                t_writer.Null();
            else {
                t_writer.StartObject();
                t_writer.EndObject();
            }
            if (!is_array)
                break;
            continue;
        }

        if (children_.empty()) {
            // --- Scalar Logic ---
            const uint8_t* p_ptr = t_arena.data() + cached_slot_->offset + current_abs_offset;
            size_t buffer_remaining = (cached_slot_->offset + cached_slot_->size) - (cached_slot_->offset + current_abs_offset);

            // For string array elements: decode_count = char capacity.
            // For scalar strings: decode_count = string_capacity_ (or count_ for legacy).
            // For non-string arrays: decode_count = 1 (one element at a time).
            // For Bool arrays: adjusted below.
            int decode_count;
            if (is_string_array) {
                decode_count = static_cast<int>(string_capacity_ > 0 ? string_capacity_ : (size_ > 2 ? size_ - 2 : 0));
            } else if (is_s7_string) {
                decode_count = static_cast<int>(string_capacity_ > 0 ? string_capacity_ : count_);
            } else {
                decode_count = is_array ? 1 : static_cast<int>(count_);
            }

            // If it's a bool array, we adjust the pointer and bit index manually to match S7 packing
            if (is_array && type_ == s7codec::Type::Bool) {
                p_ptr = t_arena.data() + cached_slot_->offset + base_abs_offset + (i / 8);
                b_idx = (i % 8);
                buffer_remaining = (cached_slot_->offset + cached_slot_->size) - (cached_slot_->offset + base_abs_offset + (i / 8));
            }

            auto dv = s7codec::decodeScalar(type_, s7codec::ReadBuffer{p_ptr, buffer_remaining},
                s7codec::DecodeOptions{//
                    .endian = endian_,
                    .bitOffset = s7codec::BitOffset{static_cast<uint8_t>(b_idx)},
                    .stringCapacity = s7codec::StringCapacity{static_cast<uint32_t>(decode_count)}});
            if (!dv.valid()) {
                t_writer.Null();
            } else {
                // Enum fields round-trip as their symbolic name — scalars
                // AND array elements (quoted JSON strings); unknown
                // numeric values fall back to the raw number.
                if (!enum_map_.empty()) {
                    const int64_t numeric = (dv.kind() == s7codec::ValueKind::UnsignedInt) ? static_cast<int64_t>(dv.u())
                                            : (dv.kind() == s7codec::ValueKind::SignedInt) ? dv.i()
                                                                                           : INT64_MIN;
                    if (numeric != INT64_MIN) {
                        auto it = enum_map_.find(static_cast<int>(numeric));
                        if (it != enum_map_.end()) {
                            t_writer.String(it->second.c_str(), static_cast<rapidjson::SizeType>(it->second.length()));
                        } else {
                            // Unknown numeric value — fall back to the number.
                            switch (dv.kind()) {
                                case s7codec::ValueKind::SignedInt:
                                    t_writer.Int64(dv.i());
                                    break;
                                case s7codec::ValueKind::UnsignedInt:
                                    t_writer.Uint64(dv.u());
                                    break;
                                default:
                                    t_writer.Null();
                                    break;
                            }
                        }
                        if (!is_array)
                            break;
                        continue;
                    }
                }
                // AFTER
                switch (dv.kind()) {
                    case s7codec::ValueKind::Bool:
                        t_writer.Bool(dv.b());
                        break;
                    case s7codec::ValueKind::SignedInt:
                        t_writer.Int64(dv.i());
                        break;
                    case s7codec::ValueKind::UnsignedInt:
                        t_writer.Uint64(dv.u());
                        break;
                    case s7codec::ValueKind::Float:
                        t_writer.Double(static_cast<double>(dv.f()));
                        break;
                    case s7codec::ValueKind::Double:
                        t_writer.Double(dv.d());
                        break;
                    case s7codec::ValueKind::String:
                        t_writer.String(dv.s().c_str(), static_cast<rapidjson::SizeType>(dv.s().length()));
                        break;
                    default:
                        t_writer.Null();
                        break;
                }
            }
        } else {
            // --- Struct Logic ---
            t_writer.StartObject();
            for (const auto& child : children_) {
                t_writer.Key(child.name_.c_str(), static_cast<rapidjson::SizeType>(child.name_.length()));
                // Recurse: pass current_abs_offset as the base for the child
                child.serialize(t_writer, t_arena, t_depth + 1, current_abs_offset);
            }
            t_writer.EndObject();
        }

        if (!is_array)
            break;
    }

    if (is_array)
        t_writer.EndArray();
}

} // namespace sgrn::gateway::twin
