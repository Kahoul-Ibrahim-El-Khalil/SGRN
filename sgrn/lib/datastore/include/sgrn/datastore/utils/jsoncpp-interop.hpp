#pragma once

// Bridge between JsonCpp (legacy DOM) and sgrn::json::Value (Glaze generic).
//
// Handlers use this at the boundary where a legacy Json::Value tree (config
// file, Garage/S3 payload) becomes a reflected response field, or where a
// reflected request field feeds JsonCpp-only code. Converting the tree
// recursively means binary wire formats carry native objects/arrays instead of
// a JSON-encoded string.

#include <sgrn/utils/json-serialization.hpp>
#include <json/json.h>

#include <cstdint>
#include <string>

namespace sgrn::datastore::utils
{

/// JsonCpp tree -> Glaze dynamic value. Scalars, arrays and objects are
/// converted recursively; null maps to JSON null.
[[nodiscard]] inline sgrn::json::Value fromJsonCpp(const Json::Value& t_value) {
    switch (t_value.type()) {
        case Json::nullValue:
            return sgrn::json::Value{};
        case Json::booleanValue:
            return sgrn::json::boolean(t_value.asBool());
        case Json::intValue:
            return sgrn::json::integer(t_value.asInt64());
        case Json::uintValue: {
            sgrn::json::Value out{};
            out = t_value.asUInt64();
            return out;
        }
        case Json::realValue:
            return sgrn::json::number(t_value.asDouble());
        case Json::stringValue:
            return sgrn::json::string(t_value.asString());
        case Json::arrayValue: {
            auto out = sgrn::json::array();
            for (const auto& element : t_value) {
                sgrn::json::append(out, fromJsonCpp(element));
            }
            return out;
        }
        case Json::objectValue: {
            auto out = sgrn::json::object();
            for (const auto& key : t_value.getMemberNames()) {
                sgrn::json::insertOrAssign(out, key, fromJsonCpp(t_value[key]));
            }
            return out;
        }
    }
    return sgrn::json::Value{};
}

/// Glaze dynamic value -> JsonCpp tree. Inverse of fromJsonCpp.
[[nodiscard]] inline Json::Value toJsonCpp(const sgrn::json::Value& t_value) {
    if (t_value.is_null()) {
        return Json::Value(Json::nullValue);
    }
    if (t_value.is_boolean()) {
        return Json::Value(t_value.get<bool>());
    }
    if (t_value.is_int64()) {
        return Json::Value(static_cast<Json::Int64>(t_value.get<std::int64_t>()));
    }
    if (t_value.is_double()) {
        return Json::Value(t_value.get<double>());
    }
    if (t_value.is_string()) {
        return Json::Value(t_value.get<std::string>());
    }
    if (t_value.is_array()) {
        Json::Value out(Json::arrayValue);
        for (const auto& element : t_value.get_array()) {
            out.append(toJsonCpp(element));
        }
        return out;
    }
    if (t_value.is_object()) {
        Json::Value out(Json::objectValue);
        for (const auto& [key, value] : t_value.get_object()) {
            out[key] = toJsonCpp(value);
        }
        return out;
    }
    return Json::Value(Json::nullValue);
}

} // namespace sgrn::datastore::utils
