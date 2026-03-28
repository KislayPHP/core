#include "kislay/runtime/async_bridge.h"

namespace kislay::runtime {

AsyncBridge::AsyncBridge()
    : event_loop_(4096)
    , worker_pool_(event_loop_, 1024) {}

AsyncBridge::~AsyncBridge() {
    stop();
}

void AsyncBridge::start(std::size_t worker_count, std::size_t runtime_lane_count) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    event_loop_.configure_lanes(runtime_lane_count);
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

bool AsyncBridge::schedule_php_task(std::size_t owner_lane, PhpTaskId task_id) {
    PhpTaskMessage message;
    message.task_id = task_id;
    message.owner_lane = owner_lane;
    return event_loop_.enqueue_php_task(std::move(message));
}

bool AsyncBridge::try_pop_result(std::size_t owner_lane, HttpResultMessage &message) {
    return event_loop_.try_pop_result(owner_lane, message);
}

bool AsyncBridge::try_pop_php_task(std::size_t owner_lane, PhpTaskMessage &message) {
    return event_loop_.try_pop_php_task(owner_lane, message);
}

bool AsyncBridge::wait_for_activity_for(std::size_t owner_lane, std::chrono::milliseconds timeout) {
    return event_loop_.wait_for_activity_for(owner_lane, timeout);
}

} // namespace kislay::runtime
