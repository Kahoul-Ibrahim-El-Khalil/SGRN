#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace sgrn::utils
{

/*
 * Example Usage:
 *
 * #include <sgrn/utils/threading.hpp>
 * #include <iostream>
 *
 * int main() {
 *     // 1. Dynamic Thread Pool (std::vector), with 10ms sleep after each task
 *     sgrn::utils::DynamicThreadPool dyn_pool(4, 10);
 *     auto f1 = dyn_pool.enqueue([]{ return 10; });
 *
 *     // Dynamically throttle thread 0 to 100ms
 *     dyn_pool.throttle(0, 100);
 *
 *     // 2. Static Thread Pool (std::array), with 0ms sleep (yields) initially
 *     sgrn::utils::StaticThreadPool<4, 0> stat_pool;
 *     auto f2 = stat_pool.enqueue([]{ return 20; });
 *
 *     std::cout << f1.get() + f2.get() << std::endl; // 30
 *     return 0;
 * }
 */

// ============================================================================
// DynamicThreadPool
// ============================================================================

/**
 * @brief A dynamic, header-only ThreadPool using std::vector.
 */
class DynamicThreadPool {
public:
    explicit DynamicThreadPool(size_t t_threads_count, size_t t_sleep_ms = 0);

    template <class F, class... Args>
    auto enqueue(F&& t_f, Args&&... t_args) -> std::future<typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    template <class F>
    void post(F&& t_f);

    /**
     * @brief Dynamically throttle a specific thread by changing its sleep duration.
     * @param thread_id The index of the worker thread.
     * @param ms The new sleep duration in milliseconds (0 means yield).
     */
    void throttle(uint16_t t_thread_id, size_t t_ms);

    ~DynamicThreadPool();

    DynamicThreadPool(const DynamicThreadPool&) = delete;
    DynamicThreadPool& operator=(const DynamicThreadPool&) = delete;
    DynamicThreadPool(DynamicThreadPool&&) = delete;
    DynamicThreadPool& operator=(DynamicThreadPool&&) = delete;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::unique_ptr<std::atomic<size_t>[]> worker_sleep_ms_;

    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

inline DynamicThreadPool::DynamicThreadPool(size_t t_threads_count, size_t t_sleep_ms)
    : worker_sleep_ms_(new std::atomic<size_t>[t_threads_count])
    , stop_(false) {
    for (size_t i = 0; i < t_threads_count; ++i) {
        worker_sleep_ms_[i].store(t_sleep_ms, std::memory_order_relaxed);
    }

    workers_.reserve(t_threads_count);
    for (size_t i = 0; i < t_threads_count; ++i) {
        workers_.emplace_back([this, i] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);
                    this->condition_.wait(lock, [this] { return this->stop_ || !this->tasks_.empty(); });
                    if (this->stop_ && this->tasks_.empty())
                        return;
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }

                task();

                size_t current_sleep = this->worker_sleep_ms_[i].load(std::memory_order_relaxed);
                if (current_sleep > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(current_sleep));
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
}

template <class F, class... Args>
auto DynamicThreadPool::enqueue(F&& t_f, Args&&... t_args)
    -> std::future<typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using return_type = typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(t_f), std::forward<Args>(t_args)...));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_)
            throw std::runtime_error("enqueue on stopped DynamicThreadPool");
        tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return res;
}

template <class F>
void DynamicThreadPool::post(F&& t_f) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_)
            throw std::runtime_error("post on stopped DynamicThreadPool");
        tasks_.emplace(std::forward<F>(t_f));
    }
    condition_.notify_one();
}

inline void DynamicThreadPool::throttle(uint16_t t_thread_id, size_t t_ms) {
    if (t_thread_id < workers_.size()) {
        worker_sleep_ms_[t_thread_id].store(t_ms, std::memory_order_relaxed);
    }
}

inline DynamicThreadPool::~DynamicThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}

// ============================================================================
// StaticThreadPool
// ============================================================================

/**
 * @brief A static, header-only ThreadPool using std::array for compile-time thread count.
 * @tparam ThreadCount The fixed number of worker threads to spawn.
 * @tparam InitialSleepMs The initial amount of milliseconds to sleep after each task. 0 means yield.
 */
template <size_t ThreadCount, size_t InitialSleepMs = 0>
class StaticThreadPool {
public:
    StaticThreadPool();

    template <class F, class... Args>
    auto enqueue(F&& t_f, Args&&... args) -> std::future<typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    template <class F>
    void post(F&& t_f);

    /**
     * @brief Dynamically throttle a specific thread by changing its sleep duration.
     * @param thread_id The index of the worker thread.
     * @param ms The new sleep duration in milliseconds (0 means yield).
     */
    void throttle(uint16_t t_thread_id, size_t t_ms);

    ~StaticThreadPool();

    StaticThreadPool(const StaticThreadPool&) = delete;
    StaticThreadPool& operator=(const StaticThreadPool&) = delete;
    StaticThreadPool(StaticThreadPool&&) = delete;
    StaticThreadPool& operator=(StaticThreadPool&&) = delete;

private:
    std::array<std::thread, ThreadCount> workers_;
    std::array<std::atomic<size_t>, ThreadCount> worker_sleep_ms_;
    std::queue<std::function<void()>> tasks_;

    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

template <size_t ThreadCount, size_t InitialSleepMs>
inline StaticThreadPool<ThreadCount, InitialSleepMs>::StaticThreadPool()
    : stop_(false) {
    for (size_t i = 0; i < ThreadCount; ++i) {
        worker_sleep_ms_[i].store(InitialSleepMs, std::memory_order_relaxed);
    }

    for (size_t i = 0; i < ThreadCount; ++i) {
        workers_[i] = std::thread([this, i] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);
                    this->condition_.wait(lock, [this] { return this->stop_ || !this->tasks_.empty(); });
                    if (this->stop_ && this->tasks_.empty())
                        return;
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }

                task();

                size_t current_sleep = this->worker_sleep_ms_[i].load(std::memory_order_relaxed);
                if (current_sleep > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(current_sleep));
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
}

template <size_t ThreadCount, size_t InitialSleepMs>
template <class F, class... Args>
auto StaticThreadPool<ThreadCount, InitialSleepMs>::enqueue(F&& t_f, Args&&... t_args)
    -> std::future<typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using return_type = typename std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(t_f), std::forward<Args>(t_args)...));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_)
            throw std::runtime_error("enqueue on stopped StaticThreadPool");
        tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return res;
}

template <size_t ThreadCount, size_t InitialSleepMs>
template <class F>
void StaticThreadPool<ThreadCount, InitialSleepMs>::post(F&& t_f) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_)
            throw std::runtime_error("post on stopped StaticThreadPool");
        tasks_.emplace(std::forward<F>(t_f));
    }
    condition_.notify_one();
}

template <size_t ThreadCount, size_t InitialSleepMs>
inline void StaticThreadPool<ThreadCount, InitialSleepMs>::throttle(uint16_t t_thread_id, size_t t_ms) {
    if (t_thread_id < ThreadCount) {
        worker_sleep_ms_[t_thread_id].store(t_ms, std::memory_order_relaxed);
    }
}

template <size_t ThreadCount, size_t InitialSleepMs>
inline StaticThreadPool<ThreadCount, InitialSleepMs>::~StaticThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}

} // namespace sgrn::utils
