#include <fmt/format.h>
#include <sgrn/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/serialization.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <stdexcept>
#include <string>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::twin
{
using sgrn::scl::DbField;
using sgrn::scl::DbSchema;
using sgrn::scl::SclError;
void serializeFieldToWriter(sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer>& t_writer, const DbField& t_field, const uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth) {
    // Delegate to the canonical S7 serialization utility (supports dirty-range
    // filtering and is templated for any Glaze JSON DOM-compatible writer).
    // Empty ranges = serialize everything (no filtering).
    sgrn::gateway::adapters::s7::serializeComplexFieldTo(t_writer, t_field, tp_ptr, t_buffer_size, {}, t_depth);
}

sgrn::Result<std::string, SclError> decodeFieldAt(
    const DbField& t_field, const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);

    serializeFieldToWriter(t_writer, t_field, tp_ptr, t_buffer_size, t_depth);
    return std::string(sb.getString());
}

std::string decodeDbBuffer(const DbSchema& t_reg, const uint8_t* tp_buf, size_t t_buffer_size) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);
    t_writer.startObject();
    for (const DbField& t_field : t_reg.fields) {
        t_writer.key(t_field.name.c_str());
        if (static_cast<size_t>(t_field.offset) >= t_buffer_size) {
            t_writer.nullValue();
            continue;
        }
        serializeFieldToWriter(t_writer, t_field, tp_buf + t_field.offset, t_buffer_size - static_cast<size_t>(t_field.offset), 0);
    }
    t_writer.endObject();
    return std::string(sb.getString());
}

sgrn::Result<std::string, SclError> decodeValue(const DbField& t_field, const uint8_t* tp_buffer_ptr, size_t t_buffer_size) {
    return decodeFieldAt(t_field, tp_buffer_ptr, t_buffer_size);
}
} // namespace sgrn::gateway::twin
