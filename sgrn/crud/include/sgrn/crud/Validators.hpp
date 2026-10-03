#pragma once

#include <sgrn/crud/CrudFieldSpec.hpp>
#include <json/json.h>
#include <string>
#include <vector>

namespace sgrn::crud
{

class Validators {
public:
    static bool validateField(const CrudFieldSpec& field, const Json::Value& value, std::string& error_out);
    static bool isValidEmail(const std::string& email);
    static bool isValidEnum(const std::string& value, const std::vector<std::string>& allowed);
};

} // namespace sgrn::crud
