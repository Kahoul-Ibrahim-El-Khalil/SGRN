#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/datastore/error/exception.hpp>
#include <sgrn/datastore/init/init.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <cstdlib>
#include <filesystem>
#include <trantor/net/EventLoopThread.h>

#include <sgrn/Result.hpp>
#include <sgrn/datastore/bootstrap/bootstrap.hpp>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/init/init.hpp>
#include <sgrn/utils/app.hpp>

using namespace sgrn::utils::app;

using namespace sgrn::datastore;

inline void init() {
    setupSignals([](int t_signum) {
        SGRN_INFO("SGRN-Datastore", "Received signal {}. Shutting down gracefully...", t_signum);
        drogon::app().quit();
    });

    assets::registerDashboardAssets();
    filters::createAndRegisterFilters();
    handlers::initHandlers();
    plugins::initMinio();

    drogon::app().run();
}

int main(int t_argc, char** tp_argv) {
    if (t_argc > 1) {
        std::string_view arg(tp_argv[1]);
        if (arg == "--help" || arg == "-h") {
            bootstrap::printHelp(tp_argv[0]);
            return EXIT_SUCCESS;
        }
        if (arg == "--init") {
            bootstrap::bootstrapDatabase();
            return EXIT_SUCCESS;
        }
        if (arg == "--generate-config") {
            std::string base_dir = sgrn::utils::filesystem::expandUserPath(std::string(bootstrap::kDefaultOperationDir));
            bootstrap::generateConfigOnly(base_dir);
            return EXIT_SUCCESS;
        }
        if (arg == "--init-db") {
            std::string base_dir = sgrn::utils::filesystem::expandUserPath(std::string(bootstrap::kDefaultOperationDir));
            bootstrap::initDatabaseOnly(base_dir);
            return EXIT_SUCCESS;
        }
        if (arg == "--config-systemd") {
            bootstrap::configureSystemd();
            return EXIT_SUCCESS;
        }
    }

    return runMain(
        t_argc, tp_argv,
        [](int t_argc, char** tp_argv) {
            ::sgrn::Result<void, std::string> configuration_result = sgrn::datastore::config::configDrogonApp(t_argc, tp_argv);
            if (configuration_result.hasError()) {
                fmt::print("{}\n", configuration_result.error());
                return EXIT_FAILURE;
            }
            init();
            return EXIT_SUCCESS;
        },
        "SGRN-Datastore");
}
