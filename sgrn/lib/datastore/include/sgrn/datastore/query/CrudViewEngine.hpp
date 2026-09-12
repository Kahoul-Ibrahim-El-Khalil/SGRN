#pragma once
#include <drogon/HttpAppFramework.h>
#include <sgrn/datastore/query/CrudViewSpec.hpp>

#include <string>

namespace sgrn::datastore::query
{

// LIST: tenant-scoped, whitelisted filters via ?col=op.value, ?order=,
// ?limit=, ?offset=. Rejects any param not in spec.fields with 400.
drogon::Task<drogon::HttpResponsePtr> executeViewList(const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant);

// GET one row by primary key, still tenant-scoped (cross-tenant id lookups
// return 404, never leak existence of another tenant's row).
drogon::Task<drogon::HttpResponsePtr> executeViewGet(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id);

// INSERT: only fields marked insertable=true are read from the JSON body;
// tenant_column is force-set from the session, never from the body.
drogon::Task<drogon::HttpResponsePtr> executeViewInsert(const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant);

// UPDATE: only fields marked updatable=true are applied; WHERE clause is
// always `pk = $id AND tenant_column = $tenant` — cannot update across tenants.
drogon::Task<drogon::HttpResponsePtr> executeViewUpdate(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id);

// DELETE: same tenant-scoped WHERE clause as UPDATE.
drogon::Task<drogon::HttpResponsePtr> executeViewDelete(
    const CrudViewSpec& t_spec, drogon::HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id);

} // namespace sgrn::datastore::query
