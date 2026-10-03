#pragma once

#include <sgrn/crud/CrudFieldSpec.hpp>
#include <sgrn/crud/CrudPolicy.hpp>

#include <drogon/HttpTypes.h>
#include <string>
#include <vector>

namespace sgrn::crud
{

struct RouteConfig {
    std::string path;
    drogon::HttpMethod method;
    std::vector<std::string> auth_filters;
};

struct ItemRouteConfig {
    std::string path;
    drogon::HttpMethod method;
    std::vector<std::string> auth_filters;
};

struct AdminOverrideSpec {
    std::string name;
    std::string path;
    std::string description;
    std::string filter_hook;
};

struct CrudViewSpec {
    std::string table_schema;
    std::string table_name;
    std::string primary_key{"id"};
    std::string primary_key_type{"int"};
    std::string tenant_column{"organisation"};
    CrudPolicy policy;
    std::vector<CrudFieldSpec> fields;
    std::string default_order{"created_at DESC"};
    std::vector<AdminOverrideSpec> admin_overrides;
};

} // namespace sgrn::crud
