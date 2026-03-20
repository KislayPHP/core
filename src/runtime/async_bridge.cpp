#include "kislay/runtime/async_bridge.h"

namespace kislay::runtime {

AsyncBridge::AsyncBridge()
    : event_loop_(4096)
    , worker_pool_(event_loop_, 1024) {}

AsyncBridge::~AsyncBridge() {
    stop();
}

void AsyncBridge::start(std::size_t worker_count) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    worker_pool_.start(worker_count);
}

void AsyncBridge::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    worker_pool_.stop();
    event_loop_.close();
}

bool AsyncBridge::running() const {
    return running_.load(std::memory_order_acquire);
}

bool AsyncBridge::submit_http(HttpRequestTask task) {
    return worker_pool_.submit(std::move(task));
}

bool AsyncBridge::schedule_php_task(PhpTaskId task_id) {
    return event_loop_.enqueue_php_task(task_id);
}

bool AsyncBridge::try_pop_result(HttpResultMessage &message) {
    return event_loop_.try_pop_result(message);
}

bool AsyncBridge::try_pop_php_task(PhpTaskId &task_id) {
    return event_loop_.try_pop_php_task(task_id);
}

bool AsyncBridge::wait_for_activity_for(std::chrono::milliseconds timeout) {
    return event_loop_.wait_for_activity_for(timeout);
}

} // namespace kislay::runtime
