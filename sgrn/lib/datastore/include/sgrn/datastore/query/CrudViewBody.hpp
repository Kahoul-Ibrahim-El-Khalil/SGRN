#pragma once

// Typed request bodies for the generated CRUD write routes.
//
// generate_views.py emits, per view, one concrete body struct for each write
// route it serves (`<View>CreateBody`, `<View>UpdateBody`): an aggregate whose
// members are the insertable / updatable columns, every member a
// std::optional.
//
// This header is deliberately declaration-only: the shared types the rest of
// the datastore needs (WriteOp, the field lookup) without Glaze. The
// Glaze-reflection write templates (parseTypedRequest / bindBody /
// auditDetailsJson) live in CrudViewBodyBind.hpp — include that only where a
// body is actually decoded or bound:
//
//   wire (JSON | MessagePack | BEVE)
//       --sgrn::parseTypedRequest<Body>-->  Body            (Glaze, no Json::Value)
//       --bindBody()-->  std::vector<BoundField>             (reflection over the members)
//       --executeView{Insert,Update}Bound()-->  SQL
//
// The wire format is the only thing that changes between clients; the struct,
// the validation (types, integer ranges) and the SQL are shared. An absent or
// null member means "not provided": the column default applies on create and
// the column is left unchanged on update.

#include <sgrn/datastore/query/CrudViewSpec.hpp>

#include <optional>
#include <string_view>
#include <type_traits>

namespace sgrn::datastore::query
{

enum class WriteOp { Insert, Update };

namespace detail
{

template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

inline const Field* findBodyField(const CrudViewSpec& t_spec, std::string_view t_name) {
    for (const auto& field : t_spec.fields) {
        if (field.name == t_name) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace detail

} // namespace sgrn::datastore::query
