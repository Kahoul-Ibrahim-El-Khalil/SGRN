#include <sgrn/Result.hpp>
#include <sgrn/datastore/bootstrap/cli.hpp>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/init/init.hpp>
#include <sgrn/utils/app.hpp>
#include <cstdlib>

using sgrn::Result;
using sgrn::datastore::config::configDrogonApp;
using sgrn::datastore::init::runServer;

int main(int t_argc, char** tp_argv) {
    // Dispatch subcommands (--init, --backup-*, --rotate, --help, …).
    // Returns the process exit code when a subcommand ran; nullopt → server mode.
    if (const std::optional<int> rc = sgrn::datastore::bootstrap::cli::dispatch(t_argc, tp_argv)) {
        return rc.value();
    }

    // Server mode: configure Drogon then start the event loop.
    return sgrn::utils::app::runMain(
        t_argc, tp_argv,
        [](int t_argc, char** tp_argv) {
            Result<void> cfg = configDrogonApp(t_argc, tp_argv);
            if (cfg.hasError()) {
                fmt::print("{}\n", cfg.error());
                return EXIT_FAILURE;
            }
            runServer();
            return EXIT_SUCCESS;
        },
        "SGRN-Datastore");
}
