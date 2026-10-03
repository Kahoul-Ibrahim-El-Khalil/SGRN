#include <sgrn/crud/Validators.hpp>

#include <algorithm>
#include <regex>

namespace sgrn::crud
{

bool Validators::isValidEmail(const std::string& email) {
    static const std::regex email_regex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    return std::regex_match(email, email_regex);
}

bool Validators::isValidEnum(const std::string& value, const std::vector<std::string>& allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

bool Validators::validateField(const CrudFieldSpec& field, const Json::Value& value, std::string& error_out) {
    if (value.isNull()) {
        if (!field.nullable) {
            error_out = "Field '" + field.name + "' cannot be null";
            return false;
        }
        return true;
    }

    // Type checks
    switch (field.type) {
        case FieldType::Int:
            if (!value.isIntegral() && !value.isNumeric()) {
                error_out = "Field '" + field.name + "' expects int";
                return false;
            }
            break;
        case FieldType::BigInt:
            if (!value.isIntegral() && !value.isNumeric() && !value.isString()) {
                error_out = "Field '" + field.name + "' expects bigint/numeric string";
                return false;
            }
            break;
        case FieldType::Text:
            if (!value.isString()) {
                error_out = "Field '" + field.name + "' expects string";
                return false;
            }
            break;
        case FieldType::Bool:
            if (!value.isBool()) {
                error_out = "Field '" + field.name + "' expects boolean";
                return false;
            }
            break;
        case FieldType::Timestamp:
            if (!value.isString()) {
                error_out = "Field '" + field.name + "' expects ISO timestamp string";
                return false;
            }
            break;
        case FieldType::Jsonb:
            // JSONB accepts any JSON value
            break;
    }

    // Validation constraints
    if (field.type == FieldType::Text && value.isString()) {
        const std::string str_val = value.asString();

        if (field.validation.maxlen > 0 && str_val.length() > field.validation.maxlen) {
            error_out = "Field '" + field.name + "' exceeds maximum length of " + std::to_string(field.validation.maxlen);
            return false;
        }

        if (field.validation.format == "email" && !isValidEmail(str_val)) {
            error_out = "Field '" + field.name + "' is not a valid email address";
            return false;
        }

        if (!field.validation.enum_values.empty() && !isValidEnum(str_val, field.validation.enum_values)) {
            error_out = "Field '" + field.name + "' must be one of permitted values";
            return false;
        }
    }

    return true;
}

} // namespace sgrn::crud
