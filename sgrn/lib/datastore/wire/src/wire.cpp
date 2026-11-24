// wire/src/wire.cpp — compile-time checks for the shared wire types.
//
// The wire headers contain only the type definitions (a struct's layout must be
// visible to every consumer that instantiates sgrn::Result<T>, readBeve<T>() or
// declares a `T body;`). They contain no function bodies. This translation unit
// verifies once, inside the wire library, that every type is a plain aggregate
// Glaze can reflect, so a bad member shows up here instead of in every client
// and server file that includes the header.

#include <sgrn/datastore/wire/wire.hpp>
#include <sgrn/utils/glaze.hpp>

namespace sgrn::datastore
{

#define SGRN_WIRE_CHECK(T)                                                                                                                 \
    static_assert(glz::reflectable<T>, #T " must be a Glaze-reflectable aggregate");                                                       \
    static_assert(std::is_default_constructible_v<T>, #T " must be default constructible");                                                \
    static_assert(std::is_move_constructible_v<T>, #T " must be move constructible")

SGRN_WIRE_CHECK(auth::SignInRequest);
SGRN_WIRE_CHECK(auth::ServiceSignInRequest);
SGRN_WIRE_CHECK(auth::UpdatePasswordRequest);
SGRN_WIRE_CHECK(auth::UserRole);
SGRN_WIRE_CHECK(auth::UserDetails);
SGRN_WIRE_CHECK(auth::UserInfo);
SGRN_WIRE_CHECK(auth::UserSignInResponse);
SGRN_WIRE_CHECK(auth::AutomatedServiceDetails);
SGRN_WIRE_CHECK(auth::ServiceSignInResponse);

SGRN_WIRE_CHECK(admin::AdminUserEntry);
SGRN_WIRE_CHECK(admin::ServiceEntry);
SGRN_WIRE_CHECK(admin::ServiceCredentials);
SGRN_WIRE_CHECK(admin::NewUser);
SGRN_WIRE_CHECK(admin::NewService);
SGRN_WIRE_CHECK(admin::ActiveSession);
SGRN_WIRE_CHECK(admin::Webhook);
SGRN_WIRE_CHECK(admin::RegisterWebhookRequest);
SGRN_WIRE_CHECK(admin::WebhookRegistration);
SGRN_WIRE_CHECK(admin::RegisteredUser);
SGRN_WIRE_CHECK(admin::RegisterUserResponse);

SGRN_WIRE_CHECK(common::Message);
SGRN_WIRE_CHECK(common::IdName);

SGRN_WIRE_CHECK(storage::DrivePathNode);
SGRN_WIRE_CHECK(storage::DriveFolderInfo);
SGRN_WIRE_CHECK(storage::DriveFileInfo);
SGRN_WIRE_CHECK(storage::DriveListing);
SGRN_WIRE_CHECK(storage::StorageUsage);
SGRN_WIRE_CHECK(storage::StorageConstraints);
SGRN_WIRE_CHECK(storage::FileVersionInfo);
SGRN_WIRE_CHECK(storage::FileVersionRestoreRequest);
SGRN_WIRE_CHECK(storage::FileVersionRestoreReceipt);
SGRN_WIRE_CHECK(storage::FileVersionTrimReceipt);
SGRN_WIRE_CHECK(storage::GarageCensus);
SGRN_WIRE_CHECK(storage::DbCensus);
SGRN_WIRE_CHECK(storage::StorageBucketCensus);
SGRN_WIRE_CHECK(storage::StorageOverview);
SGRN_WIRE_CHECK(storage::StorageOrphanKey);
SGRN_WIRE_CHECK(storage::StorageOrphanSize);
SGRN_WIRE_CHECK(storage::StorageOrphansReport);
SGRN_WIRE_CHECK(storage::PurgeSkipped);
SGRN_WIRE_CHECK(storage::PurgeKey);
SGRN_WIRE_CHECK(storage::PurgeDryRunResponse);
SGRN_WIRE_CHECK(storage::PurgeResponse);
SGRN_WIRE_CHECK(storage::ObjectInfo);

#undef SGRN_WIRE_CHECK

} // namespace sgrn::datastore
