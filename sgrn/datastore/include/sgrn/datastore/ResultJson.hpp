#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/datastore/BackendError.hpp>
#include <json/json.h>

namespace sgrn::utils::json
{

inline Json::Value toJson(const ::sgrn::datastore::BackendError& t_error) {
    Json::Value json;
    json["error"] = t_error.message_;
    json["scope"] = t_error.scope_;
    if (t_error.sub_code_.has_value()) {
        json["code"] = *t_error.sub_code_;
    }
    if (t_error.metadata_.has_value()) {
        json["metadata"] = *t_error.metadata_;
    }
    return json;
}

template <typename T>
inline Json::Value toJson(const ::sgrn::Result<T, ::sgrn::datastore::BackendError>& t_result) {
    if (t_result.has_value()) {
        if constexpr (std::is_same_v<T, void>) {
            Json::Value json;
            json["success"] = true;
            return json;
        } else if constexpr (std::is_same_v<T, Json::Value>) {
            return t_result.value();
        } else {
            Json::Value json;
            json["success"] = true;
            return json;
        }
    } else {
        return toJson(t_result.error());
    }
}

} // namespace sgrn::utils::json
