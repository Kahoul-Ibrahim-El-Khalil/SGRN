#pragma once

// Typed response rows for the generated CRUD read/write routes.
//
// generate_views.py emits, per view, one concrete `<View>Row` struct covering
// the whitelisted SELECT list (primary key, tenant column, then every spec
// field). Every member is a std::optional so a SQL NULL is preserved on the
// wire. CrudViewHandler installs a TypedRowSink; the engine fills it with the
// rows it just read/wrote, and the sink serializes them through
// createTypedResponse — JSON, MessagePack or BEVE depending on the request's
// Accept header. No Json::Value and no pre-serialized JSON strings.

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <glaze/glaze.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sgrn::datastore::query
{

// Type-erased destination for the rows a CRUD operation returns. The engine
// only needs to know how to feed rows; the concrete row type (and therefore
// the Glaze reflection used to serialize it) lives in the generated view.
class TypedRowSink {
public:
    virtual ~TypedRowSink() = default;
    virtual void add(const drogon::orm::Row& t_row) = 0;
    virtual std::string primaryKeyText(const drogon::orm::Row& t_row, std::string_view t_field_name) {
        return t_row[std::string(t_field_name)].as<std::string>();
    }
    virtual drogon::HttpResponsePtr makeResponse(const drogon::HttpRequestPtr& tsp_req, drogon::HttpStatusCode t_code) = 0;
};

namespace row_detail
{

template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <class Row, sgrn::orm::FixedString Name>
consteval std::size_t memberIndex() {
    constexpr auto keys = glz::reflect<Row>::keys;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == Name.view()) {
            return i;
        }
    }
    return keys.size();
}

template <class T>
std::string valueToText(T& t_value) {
    using Member = std::remove_cvref_t<T>;
    if constexpr (IsOptional<Member>::value) {
        return t_value ? valueToText(*t_value) : std::string{};
    } else if constexpr (std::is_same_v<Member, std::string>) {
        return t_value;
    } else if constexpr (std::is_integral_v<Member>) {
        return std::to_string(t_value);
    } else {
        static_assert(std::is_same_v<Member, void>, "Unsupported generated CRUD primary-key type");
    }
}

// Copies one Postgres text field into the reflected row member. The member
// type decides the conversion, so the generated struct and the SQL column
// type must agree (the generator derives both from the live schema).
template <class V>
void assignField(V& t_out, const drogon::orm::Field& t_field) {
    using Member = std::remove_cvref_t<V>;
    if constexpr (IsOptional<Member>::value) {
        if (t_field.isNull()) {
            t_out.reset();
        } else {
            t_out.emplace();
            assignField(*t_out, t_field);
        }
    } else {
        if (t_field.isNull()) {
            return;
        }
        if constexpr (std::is_same_v<Member, bool>) {
            t_out = t_field.as<bool>();
        } else if constexpr (std::is_same_v<Member, std::int32_t>) {
            t_out = t_field.as<std::int32_t>();
        } else if constexpr (std::is_same_v<Member, std::int64_t>) {
            t_out = t_field.as<std::int64_t>();
        } else if constexpr (std::is_same_v<Member, std::string>) {
            t_out = t_field.as<std::string>();
        } else if constexpr (std::is_same_v<Member, sgrn::json::Value>) {
            const std::string raw = t_field.as<std::string>();
            auto parsed = sgrn::json::parseValue(raw);
            if (parsed.hasError()) {
                // Corrupt JSONB text must not fail the whole read: expose it as
                // a plain string instead of silently dropping the column.
                t_out = sgrn::json::string(raw);
            } else {
                t_out = std::move(parsed).value();
            }
        }
    }
}

} // namespace row_detail

// Reflection-driven Row -> struct conversion used by the typed sinks below.
template <class Row>
Row rowFromOrm(const drogon::orm::Row& t_row) {
    Row out{};
    auto tie = glz::to_tie(out);
    constexpr auto keys = glz::reflect<Row>::keys;
    constexpr std::size_t count = glz::reflect<Row>::size;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                constexpr std::string_view key = keys[I];
                for (drogon::orm::Row::SizeType i = 0; i < t_row.size(); ++i) {
                    if (std::string_view(t_row[i].name()) == key) {
                        row_detail::assignField(glz::get<I>(tie), t_row[i]);
                        break;
                    }
                }
            }(),
            ...);
    }(std::make_index_sequence<count>{});

    return out;
}

template <class Row, sgrn::orm::FixedString Query>
Row rowFromOrm(const drogon::orm::Row& t_row) {
    using Mapping = sgrn::orm::QueryMapping<Query, Row>;
    static_assert(Mapping::num_sql_cols > 0, "Generated CRUD row projection must contain a SELECT list");
    static_assert(
        []() consteval {
            for (std::size_t i = 0; i < Mapping::num_struct_fields; ++i) {
                if (Mapping::col_indices[i] == Mapping::kNotFound) {
                    return false;
                }
            }
            return true;
        }(),
        "Generated CRUD row projection must include every reflected row field");

    if (t_row.size() != Mapping::num_sql_cols) {
        throw std::runtime_error("Generated CRUD row projection does not match the SQL result");
    }

    Row out{};
    auto tie = glz::to_tie(out);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                constexpr std::size_t column = Mapping::col_indices[I];
                row_detail::assignField(glz::get<I>(tie), t_row[static_cast<drogon::orm::Row::SizeType>(column)]);
            }(),
            ...);
    }(std::make_index_sequence<Mapping::num_struct_fields>{});
    return out;
}

template <class Row, sgrn::orm::FixedString Name>
std::string rowPrimaryKeyText(Row& t_row) {
    constexpr std::size_t index = row_detail::memberIndex<Row, Name>();
    static_assert(index < glz::reflect<Row>::size, "Generated CRUD row must contain its primary-key field");
    auto tie = glz::to_tie(t_row);
    return row_detail::valueToText(glz::get<index>(tie));
}

} // namespace sgrn::datastore::query
