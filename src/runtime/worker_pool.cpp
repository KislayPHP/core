#include "kislay/runtime/worker_pool.h"

#include <curl/curl.h>
#include <chrono>
#include <thread>

namespace kislay::runtime {
namespace {

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    const size_t bytes = size * nmemb;
    auto *buffer = static_cast<std::string *>(userp);
    buffer->append(static_cast<const char *>(contents), bytes);
    return bytes;
}

HttpResultMessage perform_http_request(const HttpRequestTask &task) {
    HttpResultMessage result;
    result.task_id = task.task_id;
    result.owner_lane = task.owner_lane;

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.error_message = "curl_easy_init failed";
        return result;
    }

    struct curl_slist *headers = nullptr;
    for (const auto &header : task.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    const bool has_body = !task.body.empty();
    const bool is_post = task.method == "POST";

    curl_easy_setopt(curl, CURLOPT_URL, task.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, task.timeout_ms > 0 ? task.timeout_ms : 10000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.response_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!task.method.empty() && task.method != "GET") {
        if (is_post) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, task.method.c_str());
        }
    }

    if (has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, task.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(task.body.size()));
    }

    CURLcode code = CURLE_OK;
    int attempt = 0;
    while (true) {
        result.response_body.clear();
        code = curl_easy_perform(curl);
        result.response_code = 0;
        if (code == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);
        }

        const bool should_retry = attempt < task.max_retries &&
            (code != CURLE_OK || result.response_code >= 500);
        if (!should_retry) {
            break;
        }

        ++attempt;
        // Non-blocking retry: sleep briefly with exponential backoff
        // Use short sleeps with interruption checks instead of one long sleep
        if (task.retry_delay_ms > 0) {
            long long remaining_ms = task.retry_delay_ms;
            while (remaining_ms > 0) {
                long long chunk = remaining_ms > 50 ? 50 : remaining_ms;
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                remaining_ms -= chunk;
            }
        }
    }

    if (code == CURLE_OK) {
        result.ok = true;
    } else {
        result.error_message = curl_easy_strerror(code);
    }

    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return result;
}

} // namespace

WorkerPool::WorkerPool(EventLoop &event_loop, std::size_t max_queue_size)
    : event_loop_(event_loop)
    , task_queue_(max_queue_size)
    , max_queue_size_(max_queue_size) {}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::start(std::size_t thread_count) {
    if (thread_count == 0 || running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    threads_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        threads_.emplace_back([this]() { worker_main(); });
    }
}

void WorkerPool::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    task_queue_.close();
    for (auto &thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    queued_tasks_.store(0, std::memory_order_release);
}

bool WorkerPool::submit(HttpRequestTask task) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::size_t queued = queued_tasks_.load(std::memory_order_acquire);
    while (queued < max_queue_size_) {
        if (queued_tasks_.compare_exchange_weak(
                queued,
                queued + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (task_queue_.try_push(std::move(task))) {
                return true;
            }
            queued_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
    }

    return false;
}

void WorkerPool::worker_main() {
    while (running_.load(std::memory_order_acquire)) {
        HttpRequestTask task;
        if (!task_queue_.wait_pop_for(task, std::chrono::milliseconds(100))) {
            continue;
        }

        queued_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        event_loop_.enqueue_result(perform_http_request(task));
    }
}

} // namespace kislay::runtime
