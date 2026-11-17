#pragma once

#include <sgrn/crud/CrudSpec.hpp>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/Field.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/ResultIterator.h>
#include <drogon/orm/Row.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/crud/Validators.hpp>
#include <sgrn/respond.hpp>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sgrn::crud
{

class RowResponseEncoder {
public:
    virtual ~RowResponseEncoder() = default;
    virtual drogon::HttpResponsePtr encodeList(const drogon::orm::Result& t_rows) const = 0;
    virtual drogon::HttpResponsePtr encodeOne(const drogon::orm::Row& t_row, drogon::HttpStatusCode t_status) const = 0;
};

struct BoundField {
    std::string name;
    std::string value;
};

namespace detail
{
template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
constexpr bool matchesFieldType(FieldType t_type) {
    if constexpr (std::is_same_v<T, int32_t>) {
        return t_type == FieldType::Int;
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return t_type == FieldType::BigInt;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return t_type == FieldType::Text || t_type == FieldType::Timestamp;
    } else if constexpr (std::is_same_v<T, bool>) {
        return t_type == FieldType::Bool;
    } else if constexpr (std::is_same_v<T, glz::generic>) {
        return t_type == FieldType::Jsonb;
    } else {
        return false;
    }
}

template <typename T>
std::string bindValue(const CrudFieldSpec& t_field, const T& t_value, std::string& t_error) {
    if (!matchesFieldType<T>(t_field.type)) {
        t_error = "Field '" + t_field.name + "' has an incompatible generated request type";
        return {};
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (t_field.type == FieldType::Timestamp && t_value.empty()) {
            t_error = "Field '" + t_field.name + "' expects ISO timestamp string";
            return {};
        }
        if (t_field.validation.maxlen > 0 && t_value.size() > t_field.validation.maxlen) {
            t_error = "Field '" + t_field.name + "' exceeds maximum length of " + std::to_string(t_field.validation.maxlen);
            return {};
        }
        if (t_field.validation.format == "email" && !Validators::isValidEmail(t_value)) {
            t_error = "Field '" + t_field.name + "' is not a valid email address";
            return {};
        }
        if (!t_field.validation.enum_values.empty() && !Validators::isValidEnum(t_value, t_field.validation.enum_values)) {
            t_error = "Field '" + t_field.name + "' must be one of permitted values";
            return {};
        }
        return t_value;
    } else if constexpr (std::is_same_v<T, bool>) {
        return t_value ? "true" : "false";
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(t_value);
    } else {
        std::string json;
        if (glz::write_json(t_value, json)) {
            t_error = "Field '" + t_field.name + "' could not be serialized as JSON";
        }
        return json;
    }
}

template <typename T>
T readResponseField(const drogon::orm::Field& t_field) {
    if constexpr (is_optional<T>::value) {
        using Value = typename T::value_type;
        if (t_field.isNull()) {
            return std::nullopt;
        }
        return T{readResponseField<Value>(t_field)};
    } else {
        if (t_field.isNull()) {
            throw std::runtime_error("Unexpected NULL in non-nullable CRUD response field");
        }
        if constexpr (std::is_same_v<T, bool>) {
            return t_field.as<bool>();
        } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
            return t_field.as<T>();
        } else if constexpr (std::is_same_v<T, glz::raw_json>) {
            return glz::raw_json{t_field.as<std::string>()};
        } else {
            return T{t_field.as<std::string>()};
        }
    }
}

template <typename T>
T responseRowFromOrm(const drogon::orm::Row& t_row) {
    T value{};
    auto tie = glz::to_tie(value);
    constexpr std::size_t kFieldCount = glz::reflect<T>::size;
    if (t_row.size() != kFieldCount) {
        throw std::runtime_error("CRUD response projection does not match generated row type");
    }
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((glz::get<I>(tie) =
                 readResponseField<std::remove_cvref_t<decltype(glz::get<I>(tie))>>(t_row[static_cast<drogon::orm::Row::SizeType>(I)])),
            ...);
    }(std::make_index_sequence<kFieldCount>{});
    return value;
}
} // namespace detail

template <typename Body>
std::optional<std::string> bindBody(const CrudViewSpec& t_spec, const Body& t_body, std::vector<BoundField>& t_out) {
    std::string error;
    constexpr std::size_t kFieldCount = glz::reflect<Body>::size;
    constexpr auto kKeys = glz::reflect<Body>::keys;
    const auto tie = glz::to_tie(t_body);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                if (!error.empty()) {
                    return;
                }
                const auto& member = glz::get<I>(tie);
                using Member = std::remove_cvref_t<decltype(member)>;
                static_assert(detail::is_optional<Member>::value, "CRUD request body fields must be optional");
                if (!member.has_value()) {
                    return;
                }

                constexpr std::string_view kName = kKeys[I];
                if (kName == t_spec.tenant_column) {
                    return;
                }
                const auto field = std::find_if(t_spec.fields.begin(), t_spec.fields.end(),
                    [kName](const CrudFieldSpec& t_candidate) { return t_candidate.name == kName; });
                const bool allowed = field != t_spec.fields.end() && field->writable && !field->soft_delete_field;
                if (!allowed) {
                    error = "Field '" + std::string(kName) + "' is not writable";
                    return;
                }
                std::string value = detail::bindValue(*field, *member, error);
                if (error.empty()) {
                    t_out.push_back({field->name, std::move(value)});
                }
            }(),
            ...);
    }(std::make_index_sequence<kFieldCount>{});

    return error.empty() ? std::nullopt : std::optional<std::string>(std::move(error));
}

template <typename T>
class TypedRowResponseEncoder final : public RowResponseEncoder {
public:
    drogon::HttpResponsePtr encodeList(const drogon::orm::Result& t_rows) const override {
        std::vector<T> response;
        response.reserve(t_rows.size());
        for (const auto& row : t_rows) {
            response.push_back(detail::responseRowFromOrm<T>(row));
        }
        return ::createTypedResponse(response);
    }

    drogon::HttpResponsePtr encodeOne(const drogon::orm::Row& t_row, drogon::HttpStatusCode t_status) const override {
        return ::createTypedResponse(detail::responseRowFromOrm<T>(t_row), t_status);
    }
};

class CrudViewEngine {
public:
    static drogon::Task<drogon::HttpResponsePtr> executeList(
        const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, const RowResponseEncoder* t_encoder = nullptr);

    static drogon::Task<drogon::HttpResponsePtr> executeGet(const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant,
        std::string id, const RowResponseEncoder* t_encoder = nullptr);

    static drogon::Task<drogon::HttpResponsePtr> executeCreate(const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant,
        const RowResponseEncoder* t_encoder = nullptr, const std::vector<BoundField>* t_bound_fields = nullptr);

    static drogon::Task<drogon::HttpResponsePtr> executeUpdate(const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant,
        std::string id, const RowResponseEncoder* t_encoder = nullptr, const std::vector<BoundField>* t_bound_fields = nullptr);

    static drogon::Task<drogon::HttpResponsePtr> executeDelete(
        const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id);
};

} // namespace sgrn::crud
