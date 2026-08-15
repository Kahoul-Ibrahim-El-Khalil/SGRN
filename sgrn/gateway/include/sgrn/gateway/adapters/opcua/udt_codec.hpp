#pragma once

#include <cstddef>
#include <cstdint>
#include <rapidjson/document.h>

struct UA_DataType;

namespace sgrn::gateway::adapters
{

bool setPrimitiveFromFieldValue(const rapidjson::Value& t_jv, const UA_DataType* tp_type, uint8_t* tp_buf);
bool getFieldValueFromPrimitive(
    const uint8_t* tp_buf, const UA_DataType* tp_type, rapidjson::Value& t_jv, rapidjson::Document::AllocatorType& t_allocator);
bool serializeUdtStructToMemory(const rapidjson::Value& t_jv, const UA_DataType& t_type, uint8_t* tp_buf);
bool deserializeMemoryToUdtJson(
    const uint8_t* tp_buf, const UA_DataType& t_type, rapidjson::Value& t_jv, rapidjson::Document::AllocatorType& t_allocator);

} // namespace sgrn::gateway::adapters
