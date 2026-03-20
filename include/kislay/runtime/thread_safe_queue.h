#pragma once

#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

namespace kislay::runtime {

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(std::size_t max_size = 0)
        : max_size_(max_size) {}
    ThreadSafeQueue(const ThreadSafeQueue &) = delete;
    ThreadSafeQueue &operator=(const ThreadSafeQueue &) = delete;

    void push(T value) {
        (void) try_push(std::move(value));
    }

    bool try_push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            if (max_size_ > 0 && queue_.size() >= max_size_) {
                return false;
            }
            queue_.push_back(std::move(value));
        }
        cv_.notify_one();
        return true;
    }

    bool try_pop(T &value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    template <typename Rep, typename Period>
    bool wait_pop_for(T &value, const std::chrono::duration<Rep, Period> &timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this]() {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    std::size_t max_size_{0};
    bool closed_{false};
};

} // namespace kislay::runtime
