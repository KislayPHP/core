#include "kislay/runtime/worker_pool.h"

#include <curl/curl.h>
#include <chrono>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

namespace kislay::runtime {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 1: Thread-local CURL handle pool
//
// Each worker thread owns a CurlPool of up to kCurlPoolCap idle handles.
// acquire() returns an idle handle (reset with curl_easy_reset) or creates a
// fresh one if the pool is empty.  release() returns the handle to the pool if
// it is not full and reuse=true; otherwise it calls curl_easy_cleanup().
// The destructor runs when the thread exits (RAII) and cleans up every handle.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr std::size_t kCurlPoolCap = 16;

struct CurlPool {
    std::vector<CURL *> idle;

    CurlPool() { idle.reserve(kCurlPoolCap); }

    ~CurlPool() {
        for (CURL *h : idle) {
            curl_easy_cleanup(h);
        }
    }

    // Returns a ready-to-configure CURL handle (never null unless OOM).
    CURL *acquire() {
        if (!idle.empty()) {
            CURL *h = idle.back();
            idle.pop_back();
            curl_easy_reset(h);  // clear all previous options before reuse
            return h;
        }
        return curl_easy_init();  // fresh handle when pool is empty
    }

    // Return handle to pool (if reuse=true and room exists) or free it.
    void release(CURL *handle, bool reuse) {
        if (reuse && idle.size() < kCurlPoolCap) {
            idle.push_back(handle);
        } else {
            curl_easy_cleanup(handle);
        }
    }
};

thread_local CurlPool tl_curl_pool;  // one pool per worker thread

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 2: Non-blocking retry via a per-thread delay queue (timer wheel)
//
// RetryTask wraps an HttpRequestTask together with an absolute wake-up time
// (fire_at).  A thread-local min-heap (tl_retry_heap) orders tasks by fire_at
// so the worker can poll the front entry each iteration without sleeping.
// ─────────────────────────────────────────────────────────────────────────────

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms        = std::chrono::milliseconds;

inline TimePoint mono_now() { return Clock::now(); }

struct RetryTask {
    HttpRequestTask task;
    TimePoint       fire_at;   // absolute time at which to execute

    // min-heap ordering: smallest fire_at at the top
    bool operator>(const RetryTask &o) const noexcept {
        return fire_at > o.fire_at;
    }
};

using RetryHeap = std::priority_queue<RetryTask,
                                      std::vector<RetryTask>,
                                      std::greater<RetryTask>>;

thread_local RetryHeap tl_retry_heap;  // per-thread delay queue

// ─────────────────────────────────────────────────────────────────────────────
// write_callback — unchanged
// ─────────────────────────────────────────────────────────────────────────────

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    const size_t bytes = size * nmemb;
    auto *buffer = static_cast<std::string *>(userp);
    buffer->append(static_cast<const char *>(contents), bytes);
    return bytes;
}

// ─────────────────────────────────────────────────────────────────────────────
// perform_http_request — single attempt; uses the thread-local CURL pool.
//
// Key differences from the old implementation:
//   • Borrows a handle from tl_curl_pool instead of curl_easy_init each time.
//   • Sets CURLOPT_FORBID_REUSE=0 and CURLOPT_TCP_KEEPALIVE=1 so libcurl can
//     reuse persistent TCP connections from its internal connection cache.
//   • Returns the handle to the pool (reuse=true) on success or HTTP error
//     where the connection is likely still alive, and discards (reuse=false)
//     on transport-level failures where the socket is probably dead.
//   • Does NOT contain the retry loop — retries are managed by worker_main()
//     via tl_retry_heap so the thread never calls sleep_for.
// ─────────────────────────────────────────────────────────────────────────────

HttpResultMessage perform_http_request(const HttpRequestTask &task) {
    HttpResultMessage result;
    result.task_id    = task.task_id;
    result.owner_lane = task.owner_lane;

    CURL *curl = tl_curl_pool.acquire();
    if (curl == nullptr) {
        result.error_message = "curl_easy_init failed";
        return result;
    }

    struct curl_slist *headers = nullptr;
    for (const auto &header : task.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    const bool has_body = !task.body.empty();
    const bool is_post  = task.method == "POST";

    // Enable libcurl-level connection reuse (set after curl_easy_reset).
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE,  0L);  // 0 = allow reuse
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);  // keep-alive probes

    curl_easy_setopt(curl, CURLOPT_URL,            task.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     task.timeout_ms > 0 ? task.timeout_ms : 10000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &result.response_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);

    if (!task.method.empty() && task.method != "GET") {
        if (is_post) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, task.method.c_str());
        }
    }

    if (has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    task.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(task.body.size()));
    }

    const CURLcode code = curl_easy_perform(curl);

    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);
        result.ok = true;
    } else {
        result.error_message = curl_easy_strerror(code);
    }

    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }

    // Reuse the handle when the transport succeeded (HTTP errors included)
    // because the TCP connection is still alive.  Discard on transport errors.
    const bool transport_ok = (code == CURLE_OK ||
                                code == CURLE_HTTP_RETURNED_ERROR);
    tl_curl_pool.release(curl, transport_ok);

    return result;
}

// Helper: execute one task (possibly a retry), decide whether to re-queue.
// Posts the result to the event loop only when retries are exhausted or
// the request ultimately succeeded.
void execute_and_maybe_retry(HttpRequestTask task, EventLoop &event_loop) {
    HttpResultMessage result = perform_http_request(task);

    const bool failed = !result.ok || result.response_code >= 500;
    const bool has_retries = task.max_retries > 0;

    if (failed && has_retries) {
        // Schedule the next attempt on the thread-local delay heap.
        RetryTask rt;
        rt.task             = std::move(task);
        rt.task.max_retries -= 1;               // one retry consumed
        const Ms delay{rt.task.retry_delay_ms > 0 ? rt.task.retry_delay_ms : 0};
        rt.fire_at          = mono_now() + delay;
        tl_retry_heap.push(std::move(rt));
        // result is NOT posted — the final outcome posts when retries finish.
    } else {
        event_loop.enqueue_result(std::move(result));
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// WorkerPool — constructor / destructor / start / stop / submit (unchanged)
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// worker_main — non-blocking retry via tl_retry_heap; no sleep_for calls.
//
// Loop structure:
//   1. Drain all due retry tasks from tl_retry_heap (fire_at <= now).
//   2. Compute the longest we can block: up to 100 ms or until the next
//      retry deadline, whichever comes first.
//   3. Call wait_pop_for with that deadline — the condvar wakes us exactly
//      when the next retry is due even if no new task arrives.
//   4. If a new task was popped, execute it (possibly scheduling a retry).
// ─────────────────────────────────────────────────────────────────────────────

void WorkerPool::worker_main() {
    while (running_.load(std::memory_order_acquire)) {
        // Step 1: execute all retry tasks whose deadline has passed.
        while (!tl_retry_heap.empty()) {
            const TimePoint fire_at = tl_retry_heap.top().fire_at;
            if (fire_at > mono_now()) {
                break;  // front is not yet due; stop draining
            }
            // Move task out (const_cast: priority_queue top is const).
            HttpRequestTask retry_task =
                std::move(const_cast<RetryTask &>(tl_retry_heap.top()).task);
            tl_retry_heap.pop();
            execute_and_maybe_retry(std::move(retry_task), event_loop_);
        }

        // Step 2: compute how long we can afford to block.
        Ms wait_ms{100};
        if (!tl_retry_heap.empty()) {
            const TimePoint next_fire = tl_retry_heap.top().fire_at;
            const auto remaining =
                std::chrono::duration_cast<Ms>(next_fire - mono_now());
            if (remaining <= Ms::zero()) {
                // Another retry is immediately due; skip blocking entirely.
                continue;
            }
            if (remaining < wait_ms) {
                wait_ms = remaining;
            }
        }

        // Step 3: block (on the condvar inside wait_pop_for) with deadline.
        // This replaces the old sleep_for: the thread either wakes because a
        // new task arrived via submit(), or because wait_ms elapsed (retry due).
        HttpRequestTask task;
        if (!task_queue_.wait_pop_for(task, wait_ms)) {
            // Timeout — loop back to step 1 to service the retry heap.
            continue;
        }

        // Step 4: new task from the queue — execute it.
        queued_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        execute_and_maybe_retry(std::move(task), event_loop_);
    }
}

} // namespace kislay::runtime
