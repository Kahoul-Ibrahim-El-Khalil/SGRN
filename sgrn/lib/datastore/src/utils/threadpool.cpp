#include <sgrn/datastore/utils/concurrency.hpp>

#include <cstdlib>
#include <memory>

namespace sgrn::utils
{

// Process-wide blocking-work pool. Replaces the former Drogon "Threadpool"
// plugin. Coroutines offload with runInPool(&sharedThreadPool(), ...) and are
// resumed on the caller's trantor::EventLoop via queueInLoop (see PoolAwaitable).
DynamicThreadPool& sharedThreadPool() {
    static const std::unique_ptr<DynamicThreadPool> pool = [] {
        int t_threads = 4;
        if (const char* t_env = std::getenv("SGRN_THREADPOOL_THREADS")) {
            try {
                int t_parsed = std::stoi(t_env);
                if (t_parsed > 0) {
                    t_threads = t_parsed;
                }
            } catch (...) {
            }
        }
        return std::make_unique<DynamicThreadPool>(static_cast<size_t>(t_threads), 0);
    }();
    return *pool;
}

} // namespace sgrn::utils