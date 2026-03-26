#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>

#include "kislay/runtime/event_loop.h"
#include "kislay/runtime/messages.h"
#include "kislay/runtime/worker_pool.h"

namespace kislay::runtime {

class AsyncBridge {
public:
    AsyncBridge();
    AsyncBridge(const AsyncBridge &) = delete;
    AsyncBridge &operator=(const AsyncBridge &) = delete;
    ~AsyncBridge();

    void start(std::size_t worker_count, std::size_t runtime_lane_count);
    void stop();
    bool running() const;

    bool submit_http(HttpRequestTask task);
    bool schedule_php_task(std::size_t owner_lane, PhpTaskId task_id);

    bool try_pop_result(std::size_t owner_lane, HttpResultMessage &message);
    bool try_pop_php_task(std::size_t owner_lane, PhpTaskMessage &message);
    bool wait_for_activity_for(std::size_t owner_lane, std::chrono::milliseconds timeout);

private:
    mutable std::mutex lifecycle_mutex_;
    EventLoop event_loop_;
    WorkerPool worker_pool_;
    std::atomic<bool> running_{false};
};

} // namespace kislay::runtime
