#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "kislay/runtime/messages.h"
#include "kislay/runtime/thread_safe_queue.h"

namespace kislay::runtime {

class EventLoop {
public:
    explicit EventLoop(std::size_t php_task_queue_size = 4096);
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    void enqueue_result(HttpResultMessage message);
    bool try_pop_result(HttpResultMessage &message);

    bool enqueue_php_task(PhpTaskId task_id);
    bool try_pop_php_task(PhpTaskId &task_id);

    void notify_activity();
    bool wait_for_activity_for(std::chrono::milliseconds timeout);
    void close();

private:
    ThreadSafeQueue<HttpResultMessage> results_;
    ThreadSafeQueue<PhpTaskId> php_tasks_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::atomic<std::uint64_t> wake_counter_{0};
    bool closed_{false};
};

} // namespace kislay::runtime
