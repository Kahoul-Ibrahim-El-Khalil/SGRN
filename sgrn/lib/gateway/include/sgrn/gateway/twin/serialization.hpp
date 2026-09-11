#pragma once
#include <sgrn/Result.hpp>
#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::twin
{
void serializeFieldToWriter(rapidjson::Writer<rapidjson::StringBuffer>& t_writer, const ::sgrn::scl::DbField& t_field,
    const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth);
sgrn::Result<std::string, ::sgrn::scl::SclError> decodeFieldAt(const ::sgrn::scl::DbField& t_field, const uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);
std::string decodeDbBuffer(const ::sgrn::scl::DbSchema& t_reg, const uint8_t* tp_buf, size_t t_buffer_size);
sgrn::Result<std::string, ::sgrn::scl::SclError> decodeValue(
    const ::sgrn::scl::DbField& t_field, const uint8_t* tp_buffer_ptr, size_t t_buffer_size);
} // namespace sgrn::gateway::twin
