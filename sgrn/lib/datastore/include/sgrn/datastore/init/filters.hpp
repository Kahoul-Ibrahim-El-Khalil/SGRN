#pragma once

#include <drogon/drogon.h>
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/filters/auth.hpp>
#include <sgrn/datastore/filters/decompression.hpp>
#include <memory>

namespace sgrn::datastore::filters
{

// Filters are registered before the handlers because the filters map is created during the filter registration.
inline void createAndRegisterFilters() {
    auto user_auth_filter_sptr = std::make_shared<UserAuthFilter>();
    auto automated_service_auth_filter_sptr = std::make_shared<AutomatedServiceAuthFilter>();
    auto admin_filter_sptr = std::make_shared<AdminFilter>();
    auto decompression_filter_sptr = std::make_shared<DecompressionFilter>();
    drogon::app().registerFilter(user_auth_filter_sptr);
    drogon::app().registerFilter(automated_service_auth_filter_sptr);
    drogon::app().registerFilter(admin_filter_sptr);
    drogon::app().registerFilter(decompression_filter_sptr);
}

} // namespace sgrn::datastore::filters
