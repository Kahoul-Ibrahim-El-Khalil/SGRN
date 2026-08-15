// src/sgrn/plugins/threadpool/Threadpool.hpp
#pragma once

#include <drogon/plugins/Plugin.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <json/json.h>
#include <optional>
#include <trantor/net/EventLoop.h>
#include <type_traits>

// ============================================================================
// sgrn::utils::runInPool  +  PoolAwaitable<T>  +  ManagedThreadPool
//
// These helpers live in sgrn::utils so that any translation unit can use them
// without pulling in the full Plugin machinery.  The Threadpool plugin itself
// is declared further below in sgrn::plugins.
// ============================================================================

#include <sgrn/datastore/utils/concurrency.hpp>

// ============================================================================
// sgrn::datastore::plugins::Threadpool  —  Drogon plugin
// ============================================================================

// ============================================================================
// sgrn::datastore::plugins::Threadpool  —  Drogon plugin
//
// Wraps a ManagedThreadPool and exposes two things to consumers:
//
//   1.  plugin->getPool()           — raw DynamicThreadPool* for runInPool()
//   2.  plugin->run(callable)       — one-shot PoolAwaitable factory
//
// Typical use in S3Client (or any coroutine):
//
//   auto *tp = drogon::app().getPlugin<sgrn::datastore::plugins::Threadpool>();
//
//   auto result = co_await tp->run([]() -> std::expected<Bytes, std::string> {
//       return blockingS3GetObject(bucket, key);
//   });
//
// JSON plugin config (drogon_ctl config):
//   {
//     "name":           "sgrn::datastore::plugins::Threadpool",
//     "dependencies":   [],
//     "config": {
//       "worker_threads":  8,
//       "pool_name":       "s3-worker-pool",
//       "worker_sleep_ms": 0
//     }
//   }
// ============================================================================

namespace sgrn::datastore::plugins
{

class Threadpool final : public drogon::Plugin<Threadpool> {
public:
    Threadpool() = default;
    ~Threadpool() override = default;

    Threadpool(const Threadpool&) = delete;
    Threadpool& operator=(const Threadpool&) = delete;
    Threadpool(Threadpool&&) = delete;
    Threadpool& operator=(Threadpool&&) = delete;

    // ── Plugin lifecycle ──────────────────────────────────────────────────────

    void initAndStart(const Json::Value& t_config) override;
    void shutdown() override;

    // ── Pool access ───────────────────────────────────────────────────────────

    /// Raw pool pointer — pass to sgrn::utils::runInPool() directly.
    sgrn::utils::DynamicThreadPool* getPool() noexcept {
        return pool_->get();
    }

    /// Convenience: create a PoolAwaitable in one call.
    ///
    ///   auto val = co_await tp->run([]() -> T { return blockingWork(); });
    template <typename F>
    auto run(F&& t_fn) -> sgrn::utils::PoolAwaitable<std::invoke_result_t<std::decay_t<F>>> {
        return sgrn::utils::runInPool(pool_->get(), std::forward<F>(t_fn));
    }

    /// Number of worker threads (set at init time).
    uint16_t getThreadCount() const noexcept {
        return thread_count_;
    }

    /// Sleep duration (set at init time).
    size_t getSleepMs() const noexcept {
        return sleep_ms_;
    }

    /// Pool name (useful for logging).
    const std::string& getPoolName() const noexcept {
        return pool_name_;
    }

    // ── Dispatch Helpers (Non-Coroutine) ──────────────────────────────────

    /**
     * @brief Offload a task to a worker thread and resume on an I/O loop.
     *
     * @tparam F Task function: std::expected<T, E> ()
     * @tparam Callback Callback function: void (std::expected<T, E> &&)
     *
     * @param t_task The blocking work to perform in the pool.
     * @param t_callback The callback to execute on the resume loop.
     * @param t_resume_loop The loop to resume on (defaults to current loop).
     */
    template <typename F, typename Callback>
    void dispatch(F&& t_task, Callback&& t_callback, trantor::EventLoop* tp_resume_loop = nullptr) {
        if (!tp_resume_loop) {
            tp_resume_loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        }

        getPool()->post(
            [t_task = std::forward<F>(t_task), t_callback = std::forward<Callback>(t_callback), tp_resume_loop = tp_resume_loop]() mutable {
                // ── Worker Thread ──
                auto result = t_task();

                // ── Resume on I/O Loop ──
                if (tp_resume_loop) {
                    tp_resume_loop->queueInLoop(
                        [res = std::move(result), callback_func = std::move(t_callback)]() mutable { callback_func(std::move(res)); });
                } else {
                    // Fallback
                    t_callback(std::move(result));
                }
            });
    }

private:
    static constexpr int kDefaultThreads = 4;
    static constexpr size_t kDefaultSleepMs = 0;
    static constexpr const char* kDefaultPoolName = "sgrn-worker-pool";

    std::string pool_name_ = kDefaultPoolName;
    int thread_count_ = kDefaultThreads;
    size_t sleep_ms_ = kDefaultSleepMs;

    // Heap-allocated so we can defer construction until initAndStart().
    std::unique_ptr<sgrn::utils::ManagedThreadPool> pool_;
};

} // namespace sgrn::datastore::plugins
