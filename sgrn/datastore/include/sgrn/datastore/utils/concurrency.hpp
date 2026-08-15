#pragma once
#include <coroutine>
#include <exception>
#include <functional>
#include <json/json.h>
#include <optional>
#include <trantor/net/EventLoop.h>
#include <type_traits>
#include <variant>

#include <sgrn/utils/threading.hpp>

namespace sgrn::utils
{

// ----------------------------------------------------------------------------
// PoolAwaitable<T>
//
// Suspends the calling coroutine, posts the blocking callable to a worker loop
// in the supplied DynamicThreadPool, then resumes the coroutine back on the
// ORIGINAL calling event loop.
// ----------------------------------------------------------------------------
template <typename T>
class PoolAwaitable {
public:
    PoolAwaitable(std::function<T()> t_task, sgrn::utils::DynamicThreadPool* tp_pool)
        : task_(std::move(t_task))
        , pool_(tp_pool) {
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> t_handle) noexcept {
        caller_loop_ = trantor::EventLoop::getEventLoopOfCurrentThread();
        handle_ = t_handle;

        pool_->post([this] {
            try {
                if constexpr (std::is_void_v<T>) {
                    task_();
                } else {
                    result_.emplace(task_());
                }
            } catch (...) {
                ex_ = std::current_exception();
            }

            auto resume = [handle_ = handle_] { handle_.resume(); };
            if (caller_loop_) {
                caller_loop_->queueInLoop(std::move(resume));
            } else {
                handle_.resume();
            }
        });
    }

    T await_resume() {
        if (ex_)
            std::rethrow_exception(ex_);
        if constexpr (!std::is_void_v<T>)
            return std::move(result_.value());
    }

private:
    std::function<T()> task_;
    sgrn::utils::DynamicThreadPool* pool_ = nullptr;
    trantor::EventLoop* caller_loop_ = nullptr;
    std::coroutine_handle<> handle_;
    std::exception_ptr ex_;

    [[no_unique_address]]
    std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> result_;
};

template <typename F>
auto runInPool(sgrn::utils::DynamicThreadPool* tp_pool, F&& t_fn) -> PoolAwaitable<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    return PoolAwaitable<R>(std::forward<F>(t_fn), tp_pool);
}

// ----------------------------------------------------------------------------
// ManagedThreadPool
// ----------------------------------------------------------------------------
class ManagedThreadPool {
public:
    explicit ManagedThreadPool(const std::string& t_name, int t_thread_count, size_t t_sleep_ms = 0)
        : pool_(t_thread_count, t_sleep_ms) {
    }

    ManagedThreadPool(const ManagedThreadPool&) = delete;
    ManagedThreadPool& operator=(const ManagedThreadPool&) = delete;

    sgrn::utils::DynamicThreadPool* get() noexcept {
        return &pool_;
    }

    /**
     * @brief Reads an integer from a JSON configuration object.
     */
    static int intFromConfig(const Json::Value& t_config, const std::string& t_key, int t_default_value) {
        if (t_config.isMember(t_key) && t_config[t_key].isInt()) {
            return t_config[t_key].asInt();
        }
        return t_default_value;
    }

private:
    sgrn::utils::DynamicThreadPool pool_;
};

} // namespace sgrn::utils
