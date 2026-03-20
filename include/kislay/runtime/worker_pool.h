#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "kislay/runtime/event_loop.h"
#include "kislay/runtime/messages.h"
#include "kislay/runtime/thread_safe_queue.h"

namespace kislay::runtime {

class WorkerPool {
public:
    explicit WorkerPool(EventLoop &event_loop, std::size_t max_queue_size = 1024);
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    ~WorkerPool();

    void start(std::size_t thread_count);
    void stop();
    bool submit(HttpRequestTask task);

private:
    void worker_main();

    EventLoop &event_loop_;
    ThreadSafeQueue<HttpRequestTask> task_queue_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> queued_tasks_{0};
    const std::size_t max_queue_size_{1024};
};

} // namespace kislay::runtime
