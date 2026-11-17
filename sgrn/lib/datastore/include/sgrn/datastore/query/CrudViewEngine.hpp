#pragma once
#include <drogon/HttpAppFramework.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/query/CrudViewRows.hpp>
#include <sgrn/datastore/query/CrudViewSpec.hpp>

#include <json/json.h>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::datastore::query
{

// Converts one JSON body value to its Postgres text bind for the given
// column type. All CRUD binds are text — Postgres casts implicitly (see
// sgrn::datastore::core::execSqlCoroVec) — so this validates the JSON type
// against the column type and renders the canonical text form (decimal
// integers, "true"/"false", ISO strings, compact JSON for jsonb).
// Mismatches (a string for an Int column, an object for a scalar, ...) are
// client errors: the result carries a message for the 400 response, never a
// silently coerced value.
sgrn::Result<std::string> fieldValueToBind(std::string_view t_field_name, FieldType t_type, const Json::Value& t_value);

// LIST: tenant-scoped, whitelisted filters via ?col=op.value, ?order=,
// ?limit=, ?offset=. Rejects any param not in spec.fields with 400.
//
// When tp_sink is non-null the result rows are converted through the sink's
// reflected row type and serialized in the request's negotiated wire format.
// Without a sink the engine keeps the legacy raw-JSON path used by the
// hand-written engine tests and dynamic-body callers.
drogon::Task<drogon::HttpResponsePtr> executeViewList(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, TypedRowSink* tp_sink = nullptr);

// GET one row by primary key, still tenant-scoped (cross-tenant id lookups
// return 404, never leak existence of another tenant's row).
drogon::Task<drogon::HttpResponsePtr> executeViewGet(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id, TypedRowSink* tp_sink = nullptr);

// INSERT / UPDATE core. The generated views decode the request body straight
// into their concrete body struct with Glaze (JSON, MessagePack or BEVE, see
// CrudViewBody.hpp) and hand the already-bound columns to these functions —
// no intermediate Json::Value. `t_audit_json` is the compact JSON object of
// the written fields (also produced by Glaze) and goes to the audit log as is.
//
// INSERT: tenant_column is force-set from the session and is always the first
// column; it can never come from the body.
drogon::Task<drogon::HttpResponsePtr> executeViewInsertBound(const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req,
    std::string t_tenant, std::vector<BoundField> t_fields, std::string t_audit_json, TypedRowSink* tp_sink = nullptr);

// UPDATE: WHERE clause is always `pk = $id AND tenant_column = $tenant` —
// cannot update across tenants. An empty field list is a 400.
drogon::Task<drogon::HttpResponsePtr> executeViewUpdateBound(const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req,
    std::string t_tenant, std::string t_id, std::vector<BoundField> t_fields, std::string t_audit_json, TypedRowSink* tp_sink = nullptr);

// Dynamic-body entry points: read the request as a JsonCpp object and walk the
// field table (insertable / updatable flags, fieldValueToBind validation).
// Generated views no longer use these — they go through the typed path above —
// they remain for hand-written callers and the engine tests, and share the
// same Bound core, so there is a single SQL implementation.
drogon::Task<drogon::HttpResponsePtr> executeViewInsert(const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant);
drogon::Task<drogon::HttpResponsePtr> executeViewUpdate(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id);

// DELETE: same tenant-scoped WHERE clause as UPDATE.
drogon::Task<drogon::HttpResponsePtr> executeViewDelete(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id);

} // namespace sgrn::datastore::query
