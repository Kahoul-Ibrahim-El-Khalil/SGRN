#include <sgrn/Result.hpp>

#include <sgrn/gateway/gateway.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using sgrn::Result;
using sgrn::gateway::GatewayApplication;
Result<void, std::string> start(int argc, char** argv) {
    GatewayApplication app;

    if (auto r = app.loadConfig(argc, argv); r.hasError()) {
        if (r.error().find("Usage") == 0) {
            fmt::print(stderr, "{}\n", r.error());
        } else {
            SGRN_ERROR_LOG("{}", r.error());
        }
        return r.error();
    }

    SGRN_IF_ERROR_PROPAGATE(app.loadSchema());
    SGRN_IF_ERROR_PROPAGATE(app.initSecurity());
    SGRN_IF_ERROR_PROPAGATE(app.initTwin());
    SGRN_IF_ERROR_PROPAGATE(app.initThreading());
    SGRN_IF_ERROR_PROPAGATE(app.wireTelemetry());
    SGRN_IF_ERROR_PROPAGATE(app.initInfrastructure());
    SGRN_IF_ERROR_PROPAGATE(app.startAdapters());

    app.feedInitialAnchor();
    app.run();
    app.shutdown();
    return {};
}
int main_cb(int argc, char** argv) {

    if (const Result<void, std::string> r = start(argc, argv); r.hasError()) {
        fmt::print(stderr, "{}\n", r.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int t_argc, char** t_argv) {
    return sgrn::utils::app::runMain(t_argc, t_argv, main_cb, "Gateway");
}
