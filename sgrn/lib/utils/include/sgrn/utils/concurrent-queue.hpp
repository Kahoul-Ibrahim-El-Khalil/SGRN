#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace sgrn::utils
{

/**
 * @brief A thread-safe concurrent queue for High-Throughput Producer/Consumer patterns.
 */
template <typename T>
class ConcurrentQueue {
public:
    ConcurrentQueue() = default;

    /**
     * @brief Push an item into the queue. Notifies one waiting consumer.
     */
    void push(const T& t_item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(t_item);
        }
        condition_.notify_one();
    }

    void push(T&& t_item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(t_item));
        }
        condition_.notify_one();
    }

    /**
     * @brief Block until an item is available and pop it.
     */
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return !queue_.empty(); });
        T t_item = std::move(queue_.front());
        queue_.pop();
        return t_item;
    }

    /**
     * @brief Try to pop an item without blocking.
     */
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty())
            return std::nullopt;
        T t_item = std::move(queue_.front());
        queue_.pop();
        return t_item;
    }

    /**
     * @brief Pop all currently available items into a batch.
     */
    std::vector<T> pop_batch(size_t t_max_items = 1000) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> batch;
        while (!queue_.empty() && batch.size() < t_max_items) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return batch;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace sgrn::utils
