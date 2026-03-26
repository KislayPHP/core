#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "kislay/runtime/messages.h"
#include "kislay/runtime/thread_safe_queue.h"

namespace kislay::runtime {

class EventLoop {
public:
    explicit EventLoop(std::size_t php_task_queue_size = 4096);
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    void configure_lanes(std::size_t lane_count);
    void enqueue_result(HttpResultMessage message);
    bool try_pop_result(std::size_t lane_index, HttpResultMessage &message);

    bool enqueue_php_task(PhpTaskMessage message);
    bool try_pop_php_task(std::size_t lane_index, PhpTaskMessage &message);

    void notify_activity(std::size_t lane_index);
    bool wait_for_activity_for(std::size_t lane_index, std::chrono::milliseconds timeout);
    void close();

private:
    struct LaneQueues {
        explicit LaneQueues(std::size_t php_task_queue_size)
            : results_()
            , php_tasks_(php_task_queue_size) {}

        ThreadSafeQueue<HttpResultMessage> results_;
        ThreadSafeQueue<PhpTaskMessage> php_tasks_;
    };

    LaneQueues &lane_queues(std::size_t lane_index);
    const LaneQueues &lane_queues(std::size_t lane_index) const;
    std::size_t normalize_lane(std::size_t lane_index) const;

    std::vector<std::unique_ptr<LaneQueues>> lanes_;
    std::size_t php_task_queue_size_{4096};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::atomic<std::uint64_t> wake_counter_{0};
    bool closed_{false};
};

} // namespace kislay::runtime
