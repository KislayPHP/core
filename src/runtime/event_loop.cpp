#include "kislay/runtime/event_loop.h"

namespace kislay::runtime {

EventLoop::EventLoop(std::size_t php_task_queue_size)
    : php_tasks_(php_task_queue_size) {}

void EventLoop::enqueue_result(HttpResultMessage message) {
    results_.push(std::move(message));
    notify_activity();
}

bool EventLoop::try_pop_result(HttpResultMessage &message) {
    return results_.try_pop(message);
}

bool EventLoop::enqueue_php_task(PhpTaskId task_id) {
    if (!php_tasks_.try_push(task_id)) {
        return false;
    }
    notify_activity();
    return true;
}

bool EventLoop::try_pop_php_task(PhpTaskId &task_id) {
    return php_tasks_.try_pop(task_id);
}

void EventLoop::notify_activity() {
    wake_counter_.fetch_add(1, std::memory_order_relaxed);
    wake_cv_.notify_all();
}

bool EventLoop::wait_for_activity_for(std::chrono::milliseconds timeout) {
    if (!results_.empty() || !php_tasks_.empty()) {
        return true;
    }
    const std::uint64_t observed = wake_counter_.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wake_mutex_);
    return wake_cv_.wait_for(lock, timeout, [this, observed]() {
        return closed_ ||
               wake_counter_.load(std::memory_order_relaxed) != observed ||
               !results_.empty() ||
               !php_tasks_.empty();
    });
}

void EventLoop::close() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        closed_ = true;
    }
    results_.close();
    php_tasks_.close();
    wake_cv_.notify_all();
}

} // namespace kislay::runtime
