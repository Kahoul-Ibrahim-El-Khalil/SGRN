// generated/RegisteredViews.cpp (auto-generated from manifests)
#include <drogon/HttpAppFramework.h>

#include "domains.gen.hpp"
#include "files.gen.hpp"
#include "user_domain_permissions.gen.hpp"
#include "users.gen.hpp"

namespace sgrn::crud::generated
{

void registerAllCrudViews() {
    drogon::app().registerController(std::make_shared<DomainsView>());
    drogon::app().registerController(std::make_shared<UserDomainPermissionsView>());
    drogon::app().registerController(std::make_shared<UsersView>());
    drogon::app().registerController(std::make_shared<FilesView>());
}

} // namespace sgrn::crud::generated
