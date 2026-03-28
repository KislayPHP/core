#include "kislay/runtime/event_loop.h"

namespace kislay::runtime {

EventLoop::EventLoop(std::size_t php_task_queue_size)
    : php_task_queue_size_(php_task_queue_size) {
    configure_lanes(1);
}

void EventLoop::configure_lanes(std::size_t lane_count) {
    if (lane_count == 0) {
        lane_count = 1;
    }

    lanes_.clear();
    lanes_.reserve(lane_count);
    for (std::size_t i = 0; i < lane_count; ++i) {
        lanes_.emplace_back(new LaneQueues(php_task_queue_size_));
    }
}

void EventLoop::enqueue_result(HttpResultMessage message) {
    lane_queues(message.owner_lane).results_.push(std::move(message));
    notify_activity(normalize_lane(message.owner_lane));
}

bool EventLoop::try_pop_result(std::size_t lane_index, HttpResultMessage &message) {
    return lane_queues(lane_index).results_.try_pop(message);
}

bool EventLoop::enqueue_php_task(PhpTaskMessage message) {
    if (!lane_queues(message.owner_lane).php_tasks_.try_push(std::move(message))) {
        return false;
    }
    notify_activity(normalize_lane(message.owner_lane));
    return true;
}

bool EventLoop::try_pop_php_task(std::size_t lane_index, PhpTaskMessage &message) {
    return lane_queues(lane_index).php_tasks_.try_pop(message);
}

void EventLoop::notify_activity(std::size_t lane_index) {
    (void) lane_index;
    wake_counter_.fetch_add(1, std::memory_order_relaxed);
    wake_cv_.notify_all();
}

bool EventLoop::wait_for_activity_for(std::size_t lane_index, std::chrono::milliseconds timeout) {
    const auto &lane = lane_queues(lane_index);
    if (!lane.results_.empty() || !lane.php_tasks_.empty()) {
        return true;
    }
    const std::uint64_t observed = wake_counter_.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wake_mutex_);
    return wake_cv_.wait_for(lock, timeout, [this, observed, lane_index]() {
        const auto &woken_lane = lane_queues(lane_index);
        return closed_ ||
               wake_counter_.load(std::memory_order_relaxed) != observed ||
               !woken_lane.results_.empty() ||
               !woken_lane.php_tasks_.empty();
    });
}

void EventLoop::close() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        closed_ = true;
    }
    for (auto &lane : lanes_) {
        if (lane) {
            lane->results_.close();
            lane->php_tasks_.close();
        }
    }
    wake_cv_.notify_all();
}

EventLoop::LaneQueues &EventLoop::lane_queues(std::size_t lane_index) {
    return *lanes_[normalize_lane(lane_index)];
}

const EventLoop::LaneQueues &EventLoop::lane_queues(std::size_t lane_index) const {
    return *lanes_[normalize_lane(lane_index)];
}

std::size_t EventLoop::normalize_lane(std::size_t lane_index) const {
    if (lanes_.empty()) {
        return 0;
    }
    return lane_index < lanes_.size() ? lane_index : 0;
}

} // namespace kislay::runtime
