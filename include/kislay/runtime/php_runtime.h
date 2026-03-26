#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "kislay/runtime/messages.h"
#include "kislay/runtime/thread_safe_queue.h"

namespace kislay::runtime {

enum class PhpRuntimeMode {
    SingleThreadLoop,
    DedicatedThreadLoop,
    ZtsParallel
};

struct PhpRuntimeConfig {
    bool is_zts{false};
    bool parallel_enabled{false};
    bool background_thread{false};
    std::size_t runtime_threads{1};
    std::size_t request_queue_size{2048};
    std::uint32_t max_requests{0};
};

class RequestCompletion {
public:
    RequestCompletion() = default;

    void complete(RuntimeResponseMessage response);
    bool wait_for(RuntimeResponseMessage &response, std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    RuntimeResponseMessage response_;
};

using RuntimeRequestHandler = std::function<void(std::size_t, RuntimeRequestMessage &, RuntimeResponseMessage &)>;

class PhpRuntimePool {
public:
    PhpRuntimePool(PhpRuntimeConfig config, RuntimeRequestHandler handler);
    PhpRuntimePool(const PhpRuntimePool &) = delete;
    PhpRuntimePool &operator=(const PhpRuntimePool &) = delete;
    ~PhpRuntimePool();

    bool start();
    void stop();

    bool submit(RuntimeRequestMessage request);
    std::size_t drain(std::size_t budget);
    bool wait_for_activity_for(std::chrono::milliseconds timeout);
    bool has_pending_requests() const;

    PhpRuntimeMode mode() const { return mode_; }
    std::size_t runtime_threads() const { return runtime_threads_; }
    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void worker_main(std::size_t runtime_index);
    void supervisor_main(std::size_t runtime_index);
    void notify_activity();

    PhpRuntimeConfig config_;
    RuntimeRequestHandler handler_;
    ThreadSafeQueue<RuntimeRequestMessage> request_queue_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::atomic<std::uint64_t> wake_counter_{0};
    std::atomic<bool> running_{false};
    std::vector<std::thread> runtime_threads_storage_;
    PhpRuntimeMode mode_{PhpRuntimeMode::SingleThreadLoop};
    std::size_t runtime_threads_{1};
};

} // namespace kislay::runtime
