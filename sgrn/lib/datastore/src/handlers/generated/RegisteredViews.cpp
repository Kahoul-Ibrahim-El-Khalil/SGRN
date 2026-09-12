// Hand-curated after first generation. This file is only ever
// APPENDED to — a line you delete here stays deleted, even after
// re-running the generator. If a table is later dropped from the
// database, its line here will fail to compile — delete it then.
//
// Each view self-registers its routes in its constructor (the same
// IHandler mechanism every other handler uses). The function-local
// statics below mirror initHandlers(): they are constructed exactly once,
// on the first initGeneratedViews() call from initHandlers(), i.e. after
// config load and before drogon::app().run().
#include "AllViews.gen.hpp"

namespace sgrn::datastore::handlers::query
{

void initGeneratedViews() {
    static DomainsView s_domains;
    static UsersView s_users;
    static AutomatedServicesView s_automated_services;
    static UserDomainPermissionsView s_user_domain_permissions;
}

} // namespace sgrn::datastore::handlers::query
