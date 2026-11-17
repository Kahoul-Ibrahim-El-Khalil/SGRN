#pragma once

// Glaze reflection over the generated CRUD request bodies: the write-route
// templates. Split from CrudViewBody.hpp so that code which only passes the
// spec / BoundField types around never parses <glaze/glaze.hpp>; include this
// only where a body is actually decoded or bound.
//
//   wire (JSON | MessagePack | BEVE)
//       --sgrn::parseTypedRequest<Body>-->  Body            (Glaze, no Json::Value)
//       --bindBody()-->  std::vector<BoundField>             (reflection over the members)
//       --executeView{Insert,Update}Bound()-->  SQL

#include <sgrn/Result.hpp>
#include <sgrn/datastore/query/CrudViewBody.hpp>

#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sgrn::datastore::query
{

namespace detail
{

// The C++ member type a column type is generated as. Checked at runtime
// against the view's field table so a stale generated header (struct and
// spec out of sync) fails loudly instead of binding the wrong thing.
template <class V>
constexpr bool matchesFieldType(FieldType t_type) {
    if constexpr (std::is_same_v<V, bool>) {
        return t_type == FieldType::Bool;
    } else if constexpr (std::is_same_v<V, std::string>) {
        return t_type == FieldType::Text || t_type == FieldType::Timestamp;
    } else if constexpr (std::is_same_v<V, int32_t>) {
        return t_type == FieldType::Int;
    } else if constexpr (std::is_same_v<V, int64_t>) {
        return t_type == FieldType::BigInt;
    } else if constexpr (std::is_same_v<V, glz::generic>) {
        return t_type == FieldType::Jsonb;
    } else {
        return false;
    }
}

// Renders one member as the text bind Postgres receives: canonical decimal
// integers, "true"/"false", the string itself, compact JSON for jsonb.
// Glaze has already checked the wire type and the integer range while
// decoding, so only what the type system cannot express is checked here.
template <class V>
sgrn::Result<std::string> renderBind(std::string_view t_name, const Field& t_field, const V& t_value) {
    if (!matchesFieldType<V>(t_field.type)) {
        return ::sgrn::Error(fmt::format("Field '{}': body type does not match the column type (regenerate the CRUD views)", t_name));
    }
    if constexpr (std::is_same_v<V, std::string>) {
        // Timestamps are ISO strings; an empty one can never be valid.
        if (t_field.type == FieldType::Timestamp && t_value.empty()) {
            return ::sgrn::Error(fmt::format("Field '{}' expects timestamp but got an empty string", t_name));
        }
        return t_value;
    } else if constexpr (std::is_same_v<V, bool>) {
        return std::string(t_value ? "true" : "false");
    } else if constexpr (std::is_integral_v<V>) {
        return std::to_string(t_value);
    } else {
        std::string json;
        if (glz::write_json(t_value, json)) {
            return ::sgrn::Error(fmt::format("Field '{}' could not be serialized as JSON", t_name));
        }
        return json;
    }
}

} // namespace detail

// Walks the body struct with Glaze reflection and returns one BoundField per
// member that holds a value. Each member is checked against the view's field
// table: it must exist and carry the insertable / updatable flag for `t_op`,
// and the tenant column is never written, whatever the struct contains.
template <class Body>
sgrn::Result<std::vector<BoundField>> bindBody(const CrudViewSpec& t_spec, const Body& t_body, WriteOp t_op) {
    std::vector<BoundField> out;
    std::string error;

    constexpr std::size_t N = glz::reflect<Body>::size;
    if constexpr (N > 0) {
        const auto tie = glz::to_tie(t_body);
        constexpr auto keys = glz::reflect<Body>::keys;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&]() {
                    if (!error.empty()) {
                        return;
                    }
                    const auto& member = glz::get<I>(tie);
                    using Member = std::remove_cvref_t<decltype(member)>;
                    static_assert(detail::IsOptional<Member>::value, "CRUD body members must be std::optional");
                    if (!member.has_value()) {
                        return;
                    }
                    constexpr std::string_view key = keys[I];
                    if (key == t_spec.tenant_column) {
                        return;
                    }
                    const Field* field = detail::findBodyField(t_spec, key);
                    const bool allowed = field != nullptr && (t_op == WriteOp::Insert ? field->insertable : field->updatable);
                    if (!allowed) {
                        error = fmt::format("Field '{}' is not writable", key);
                        return;
                    }
                    auto bind_res = detail::renderBind(key, *field, *member);
                    if (bind_res.hasError()) {
                        error = bind_res.error();
                        return;
                    }
                    out.push_back({field->name, std::move(bind_res).value()});
                }(),
                ...);
        }(std::make_index_sequence<N>{});
    }

    if (!error.empty()) {
        return ::sgrn::Error(std::move(error));
    }
    return out;
}

// The written columns as a compact JSON object, for the audit record. Glaze
// omits empty optionals, so this is exactly the fields the client sent.
template <class Body>
std::string auditDetailsJson(const Body& t_body) {
    std::string json;
    if (glz::write_json(t_body, json)) {
        return "{}";
    }
    return json;
}

} // namespace sgrn::datastore::query
