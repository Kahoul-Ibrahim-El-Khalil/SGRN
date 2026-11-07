#pragma once

#include "assets.hpp"
#include "filters.hpp"
#include "handlers.hpp"
#include "rate-limit.hpp"
#include "s3.hpp"

#include <drogon/drogon.h>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/services/upload-cleanup.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/app.hpp>

namespace sgrn::datastore::init
{

/**
 * @brief Register all Drogon assets, filters, handlers, plugins, rate-limiters
 *        and background services, then start the event loop.
 *
 * Called from main() after Drogon has been configured and all CLI subcommands
 * have been dispatched.  Does not return until the application shuts down.
 */
inline void runServer() {
    sgrn::utils::app::setupSignals([](int t_signum) {
        SGRN_INFO("SGRN-Datastore", "Received signal {}. Shutting down gracefully...", t_signum);
        drogon::app().quit();
    });

    assets::registerDashboardAssets();
    filters::createAndRegisterFilters();
    handlers::initHandlers();
    crypto::initKeyStore();
    plugins::initS3();
    ratelimit::initRateLimiting();
    services::upload_cleanup::startUploadCleanupJob();

    drogon::app().run();
}

} // namespace sgrn::datastore::init
