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

    void start(std::size_t worker_count);
    void stop();
    bool running() const;

    bool submit_http(HttpRequestTask task);
    bool schedule_php_task(PhpTaskId task_id);

    bool try_pop_result(HttpResultMessage &message);
    bool try_pop_php_task(PhpTaskId &task_id);
    bool wait_for_activity_for(std::chrono::milliseconds timeout);

private:
    mutable std::mutex lifecycle_mutex_;
    EventLoop event_loop_;
    WorkerPool worker_pool_;
    std::atomic<bool> running_{false};
};

} // namespace kislay::runtime
