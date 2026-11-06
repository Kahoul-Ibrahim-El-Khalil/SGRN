#pragma once

/**
 * @file json-helper.hpp
 * @brief Forwarding header for RapidJSON utilities now living in sgrn::utils::rapidjson.
 */

#include <sgrn/utils/rapidjson.hpp>

namespace sgrn::common::json_helper
{

using sgrn::utils::rapidjson::buildArrayResponse;
using sgrn::utils::rapidjson::copyJsonPath;
using sgrn::utils::rapidjson::extractField;
using sgrn::utils::rapidjson::filterFields;
using sgrn::utils::rapidjson::parse;
using sgrn::utils::rapidjson::serializeObject;
using sgrn::utils::rapidjson::serializeValue;

} // namespace sgrn::common::json_helper
