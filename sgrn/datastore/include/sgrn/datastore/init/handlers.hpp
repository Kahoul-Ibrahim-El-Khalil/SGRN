#pragma once

#include <sgrn/datastore/handlers/PostgrestProxy.hpp>
#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/handlers/auth.hpp>
#include <sgrn/datastore/handlers/query.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/handlers/telemetry.hpp>

namespace sgrn::datastore::handlers
{

// the constructor of the handlers defines the route -> handler map;
// We declare the handlers static to ensure they are not destroyed before the end of the program.
inline void initHandlers() {
    using namespace sgrn::datastore::handlers;
    static auth::AuthApiHandler auth_api_handler;
    static admin::AdminApiHandler admin_api_handler;
    static query::QueryApiHandler query_api_handler;
    static storage::StorageApiHandler storage_api_handler;
    static telemetry::TelemetryHandler telemetry_handler;
    static query::PostgrestProxyHandler postgrest_proxy_handler;
}

} // namespace sgrn::datastore::handlers
