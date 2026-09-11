#pragma once

#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sgrn::utils
{

template <typename T>
class EventBroker {
public:
    using SubscriberCallback = std::function<void(const T&)>;

    explicit EventBroker(asio::io_context& t_io)
        : io_(t_io) {
    }

    size_t subscribe(SubscriberCallback t_cb) {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        size_t t_id = next_id_++;
        subscribers_[t_id] = std::move(t_cb);
        return t_id;
    }

    void unsubscribe(size_t t_id) {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        subscribers_.erase(t_id);
    }

    void publish(T t_event) {
        std::lock_guard<std::mutex> lock(sub_mutex_);

        // Dispatch asynchronously via asio event loop
        asio::post(io_, [this, ev = std::move(t_event)]() mutable { dispatch(ev); });
    }

private:
    void dispatch(const T& t_event) {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        for (auto& [t_id, t_cb] : subscribers_) {
            t_cb(t_event);
        }
    }

private:
    asio::io_context& io_;

    std::unordered_map<size_t, SubscriberCallback> subscribers_;
    std::mutex sub_mutex_;
    std::atomic<size_t> next_id_{0};
};

} // namespace sgrn::utils
