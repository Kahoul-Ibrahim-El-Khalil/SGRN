#include <regex>
// src/sgrn/plugins/threadpool/Threadpool.cpp
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>

#include <sgrn/debug.hpp>
#include <trantor/utils/Logger.h>

#ifdef DEBUG_PLUGIN_THREADPOOL
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("ThreadPool", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("ThreadPool", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("ThreadPool", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("ThreadPool", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::plugins
{

// ── initAndStart ──────────────────────────────────────────────────────────────
//
// Called once by Drogon before the server starts accepting requests.
// Reads optional JSON config keys:
//
//   "worker_threads"  (int)    — number of worker threads  [default: 4]
//   "pool_name"       (string) — name shown in logs         [default: "sgrn-worker-pool"]
//   "worker_sleep_ms" (int)    — sleep ms after each task   [default: 0]
//
void Threadpool::initAndStart(const Json::Value& t_config) {
    // ── Read config ───────────────────────────────────────────────────────────
    thread_count_ = sgrn::utils::ManagedThreadPool::intFromConfig(t_config, "worker_threads", kDefaultThreads);
    sleep_ms_ = sgrn::utils::ManagedThreadPool::intFromConfig(t_config, "worker_sleep_ms", 0);

    if (t_config.isMember("pool_name") && t_config["pool_name"].isString()) {
        pool_name_ = t_config["pool_name"].asString();
    }

    // ── Start the pool ────────────────────────────────────────────────────────
    pool_ = std::make_unique<sgrn::utils::ManagedThreadPool>(pool_name_, thread_count_, sleep_ms_);

    INFO_LOG("'{}' started with {} worker thread(s) (sleep: {} ms).", pool_name_, thread_count_, sleep_ms_);
}

// ── shutdown ──────────────────────────────────────────────────────────────────
//
// Called by Drogon during graceful shutdown (before the process exits).
// Destroying the ManagedThreadPool causes the threadpool to quit each
// loop and join all worker threads.
//
void Threadpool::shutdown() {
    pool_.reset(); // joins all threads
    INFO_LOG("'{}' shut down.", pool_name_);
}

} // namespace sgrn::datastore::plugins

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
