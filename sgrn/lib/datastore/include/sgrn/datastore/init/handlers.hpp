#pragma once

#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/handlers/auth.hpp>
#include <sgrn/datastore/handlers/query.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/handlers/storage_admin.hpp>

// Generated CRUD views (compile-time REST-over-Postgres). The header only
// declares initGeneratedViews(); the per-table instances live in the
// generated RegisteredViews.cpp, compiled once into sgrn_datastore_lib.
#include <handlers/generated/RegisteredViews.hpp>

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
    static storage_admin::StorageAdminHandler storage_admin_handler;

    // Compile-time generated CRUD views (one static instance per table;
    // see src/handlers/generated/RegisteredViews.cpp).
    query::initGeneratedViews();
}

} // namespace sgrn::datastore::handlers
