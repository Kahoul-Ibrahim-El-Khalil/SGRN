#include <fmt/format.h>
#include <sgrn/gateway/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/serialization.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DataBlockRegistry;
using ::sgrn::scl::DbField;

void serializeFieldToWriter(rapidjson::Writer<rapidjson::StringBuffer>& t_writer, const DbField& t_field, const uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth) {
    // Delegate to the canonical S7 serialization utility (supports dirty-range
    // filtering and is templated for any RapidJSON-compatible writer).
    // Empty ranges = serialize everything (no filtering).
    sgrn::gateway::adapters::s7::serializeComplexFieldTo(t_writer, t_field, tp_ptr, t_buffer_size, {}, t_depth);
}

sgrn::Result<std::string, ::sgrn::scl::Error> decodeFieldAt(
    const DbField& t_field, const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);

    // We can just use serializeFieldToWriter, but it currently isn't accessible here.
    // Wait, serializeFieldToWriter is exactly what we need, I'll copy it here below.
    serializeFieldToWriter(t_writer, t_field, tp_ptr, t_buffer_size, t_depth);
    return std::string(sb.GetString());
}

std::string decodeDbBuffer(const DataBlockRegistry& t_reg, const uint8_t* tp_buf, size_t t_buffer_size) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    t_writer.StartObject();
    for (const DbField& t_field : t_reg.fields) {
        t_writer.Key(t_field.name.c_str());
        if (static_cast<size_t>(t_field.offset) >= t_buffer_size) {
            t_writer.Null();
            continue;
        }
        serializeFieldToWriter(t_writer, t_field, tp_buf + t_field.offset, t_buffer_size - static_cast<size_t>(t_field.offset), 0);
    }
    t_writer.EndObject();
    return std::string(sb.GetString());
}

sgrn::Result<std::string, ::sgrn::scl::Error> decodeValue(const DbField& t_field, const uint8_t* tp_buffer_ptr, size_t t_buffer_size) {
    return decodeFieldAt(t_field, tp_buffer_ptr, t_buffer_size);
}
} // namespace sgrn::gateway::twin
