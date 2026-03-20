#include "kislay/runtime/php_runtime.h"

namespace kislay::runtime {

void RequestCompletion::complete(RuntimeResponseMessage response) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_) {
            return;
        }
        response_ = std::move(response);
        ready_ = true;
    }
    cv_.notify_all();
}

bool RequestCompletion::wait_for(RuntimeResponseMessage &response, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this]() { return ready_; })) {
        return false;
    }
    response = std::move(response_);
    return true;
}

PhpRuntimePool::PhpRuntimePool(PhpRuntimeConfig config, RuntimeRequestHandler handler)
    : config_(std::move(config))
    , handler_(std::move(handler))
    , request_queue_(config_.request_queue_size) {
    if (config_.parallel_enabled && config_.is_zts) {
        mode_ = PhpRuntimeMode::ZtsParallel;
    } else if (config_.background_thread && config_.is_zts) {
        mode_ = PhpRuntimeMode::DedicatedThreadLoop;
    } else {
        mode_ = PhpRuntimeMode::SingleThreadLoop;
    }
    runtime_threads_ = mode_ == PhpRuntimeMode::ZtsParallel
        ? (config_.runtime_threads == 0 ? 1 : config_.runtime_threads)
        : 1;
}

PhpRuntimePool::~PhpRuntimePool() {
    stop();
}

bool PhpRuntimePool::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    if (mode_ != PhpRuntimeMode::SingleThreadLoop) {
        runtime_threads_storage_.reserve(runtime_threads_);
        for (std::size_t i = 0; i < runtime_threads_; ++i) {
            runtime_threads_storage_.emplace_back([this, i]() { worker_main(i); });
        }
    }

    notify_activity();
    return true;
}

void PhpRuntimePool::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    request_queue_.close();
    notify_activity();

    for (auto &thread : runtime_threads_storage_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    runtime_threads_storage_.clear();
}

bool PhpRuntimePool::submit(RuntimeRequestMessage request) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    const bool pushed = request_queue_.try_push(std::move(request));
    if (pushed) {
        notify_activity();
    }
    return pushed;
}

std::size_t PhpRuntimePool::drain(std::size_t budget) {
    if (mode_ != PhpRuntimeMode::SingleThreadLoop || !running_.load(std::memory_order_acquire)) {
        return 0;
    }

    std::size_t processed = 0;
    while (processed < budget) {
        RuntimeRequestMessage request;
        if (!request_queue_.try_pop(request)) {
            break;
        }

        RuntimeResponseMessage response;
        response.task_id = request.task_id;
        if (handler_) {
            handler_(0, request, response);
        } else {
            response.status_code = 500;
            response.body = "PHP runtime handler is not configured";
            response.request_error = response.body;
        }
        if (request.completion) {
            request.completion->complete(std::move(response));
        }
        ++processed;
    }

    return processed;
}

bool PhpRuntimePool::wait_for_activity_for(std::chrono::milliseconds timeout) {
    if (!request_queue_.empty()) {
        return true;
    }
    const std::uint64_t observed = wake_counter_.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wake_mutex_);
    return wake_cv_.wait_for(lock, timeout, [this, observed]() {
        return !running_.load(std::memory_order_acquire) ||
               wake_counter_.load(std::memory_order_relaxed) != observed ||
               !request_queue_.empty();
    });
}

bool PhpRuntimePool::has_pending_requests() const {
    return !request_queue_.empty();
}

void PhpRuntimePool::worker_main(std::size_t runtime_index) {
    while (running_.load(std::memory_order_acquire)) {
        RuntimeRequestMessage request;
        if (!request_queue_.wait_pop_for(request, std::chrono::milliseconds(100))) {
            continue;
        }

        RuntimeResponseMessage response;
        response.task_id = request.task_id;
        if (handler_) {
            handler_(runtime_index, request, response);
        } else {
            response.status_code = 500;
            response.body = "PHP runtime handler is not configured";
            response.request_error = response.body;
        }
        if (request.completion) {
            request.completion->complete(std::move(response));
        }
    }
}

void PhpRuntimePool::notify_activity() {
    wake_counter_.fetch_add(1, std::memory_order_relaxed);
    wake_cv_.notify_all();
}

} // namespace kislay::runtime
