extern "C" {
#include "php.h"
#include "SAPI.h"
#include "php_main.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "ext/json/php_json.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_gc.h"
#include "Zend/zend_smart_str.h"

#if defined(ZTS)
ZEND_TSRMLS_CACHE_EXTERN();
#endif
}

#include <curl/curl.h>
#include <openssl/rand.h>
#include <thread>
#include <future>
#include <memory>

#if defined(_WIN32)
#include <bcrypt.h>
#endif

#include "php_kislay_extension.h"

#include <civetweb.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <csignal>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <map>
#include <vector>
#include <deque>
#include <condition_variable>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#include "kislay/runtime/async_bridge.h"
#include "kislay/runtime/php_runtime.h"
#include "kislay/runtime/uv_server.h"

typedef struct _php_kislay_app_t php_kislay_app_t;
typedef struct _php_kislay_async_http_t php_kislay_async_http_t;
static thread_local bool kislay_php_thread_active = false;
static thread_local std::size_t kislay_php_runtime_lane_index = 0;
static bool kislay_call_php(zval *callable, uint32_t argc, zval *argv, zval *retval, std::string *error_out = nullptr);
static bool kislay_is_controller_handler(zval *handler);
static bool kislay_normalize_controller_callable(zval *handler, zval *out);
static void kislay_release_async_http(php_kislay_async_http_t *async_http);
static bool kislay_jwt_verify_hs256(const std::string &header_b64, const std::string &payload_b64,
                                    const std::string &sig_b64, const std::string &secret);
static bool kislay_jwt_parse_payload(const std::string &payload_b64, zval *out);
static bool kislay_dispatch_to_subapp(zval *subapp_obj, const std::string &method, const std::string &stripped_uri, zval *req_obj, zval *res_obj);

enum class PromiseState {
    Pending,
    Fulfilled,
    Rejected
};

typedef struct _php_kislay_promise_t {
    PromiseState state;
    zval result;
    php_kislay_app_t *owner_app;
    std::string owner_request_id;
    kislay::runtime::TaskId async_id;
    std::size_t owner_lane;
    zend_object std;
} php_kislay_promise_t;

struct KislayPendingPhpTask {
    zval callable;
    std::string request_id;
};

static inline php_kislay_promise_t *php_kislay_promise_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_promise_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_promise_t, std));
}

namespace kislay::runtime {

class PromiseRegistry {
public:
    PromiseRegistry() = default;

    void register_promise(TaskId task_id, php_kislay_promise_t *promise) {
        ZEND_ASSERT(promise != nullptr);
        GC_ADDREF(&promise->std);
        promises_[task_id] = promise;
    }

    php_kislay_promise_t *get_promise(TaskId task_id) {
        auto it = promises_.find(task_id);
        return it == promises_.end() ? nullptr : it->second;
    }

    void unregister_promise(TaskId task_id) {
        auto it = promises_.find(task_id);
        if (it != promises_.end()) {
            OBJ_RELEASE(&it->second->std);
            promises_.erase(it);
        }
        remove_callbacks(task_id, fulfilled_callbacks_);
        remove_callbacks(task_id, rejected_callbacks_);
    }

    void add_fulfilled_callback(TaskId task_id, zval *callback) {
        ZEND_ASSERT(callback != nullptr);
        zval copy;
        ZVAL_COPY_VALUE(&copy, callback);
        Z_TRY_ADDREF_P(&copy);
        ZEND_ASSERT(!Z_REFCOUNTED(copy) || GC_REFCOUNT(Z_COUNTED(copy)) >= 1);
        fulfilled_callbacks_.emplace(task_id, copy);
    }

    void add_rejected_callback(TaskId task_id, zval *callback) {
        ZEND_ASSERT(callback != nullptr);
        zval copy;
        ZVAL_COPY_VALUE(&copy, callback);
        Z_TRY_ADDREF_P(&copy);
        ZEND_ASSERT(!Z_REFCOUNTED(copy) || GC_REFCOUNT(Z_COUNTED(copy)) >= 1);
        rejected_callbacks_.emplace(task_id, copy);
    }

    void add_finally_callback(TaskId task_id, zval *callback) {
        add_fulfilled_callback(task_id, callback);
        add_rejected_callback(task_id, callback);
    }

    void dispatch(php_kislay_promise_t *promise) {
        if (promise == nullptr || promise->state == PromiseState::Pending) {
            return;
        }

        auto &callbacks = promise->state == PromiseState::Fulfilled ? fulfilled_callbacks_ : rejected_callbacks_;
        auto range = callbacks.equal_range(promise->async_id);
        for (auto it = range.first; it != range.second; ++it) {
            zval args[1];
            ZVAL_COPY(&args[0], &promise->result);
            zval retval;
            ZVAL_UNDEF(&retval);
            if (kislay_call_php(&it->second, 1, args, &retval)) {
                zval_ptr_dtor(&retval);
            }
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&it->second);
        }
        callbacks.erase(range.first, range.second);

        auto &other = promise->state == PromiseState::Fulfilled ? rejected_callbacks_ : fulfilled_callbacks_;
        remove_callbacks(promise->async_id, other);
    }

    void clear() {
        clear_callbacks(fulfilled_callbacks_);
        clear_callbacks(rejected_callbacks_);
        for (auto &entry : promises_) {
            OBJ_RELEASE(&entry.second->std);
        }
        promises_.clear();
    }

private:
    using CallbackMap = std::unordered_multimap<TaskId, zval>;

    static void clear_callbacks(CallbackMap &callbacks) {
        for (auto &entry : callbacks) {
            zval_ptr_dtor(&entry.second);
        }
        callbacks.clear();
    }

    static void remove_callbacks(TaskId task_id, CallbackMap &callbacks) {
        auto range = callbacks.equal_range(task_id);
        for (auto it = range.first; it != range.second; ++it) {
            zval_ptr_dtor(&it->second);
        }
        callbacks.erase(range.first, range.second);
    }

    std::unordered_map<TaskId, php_kislay_promise_t *> promises_;
    CallbackMap fulfilled_callbacks_;
    CallbackMap rejected_callbacks_;
};

} // namespace kislay::runtime

#if defined(ZTS)
ZEND_TSRMLS_CACHE_EXTERN();
#endif

namespace kislay {

struct RequestField {
    std::string_view key;
    std::string_view value;
};

struct RequestAttribute {
    std::string key;
    zval value;
};

struct Route {
    struct Segment {
        std::string value;
        bool is_param{false};
    };

    std::string method;
    std::string pattern;
    std::vector<Segment> segments;
    std::vector<std::string> param_names;
    std::unordered_map<std::string, std::size_t> param_index_map;
    std::vector<zval> middleware;
    std::vector<zval> compiled_middleware;
    zval handler;
    bool exact{false};
};

struct GroupContext {
    std::string prefix;
    std::vector<zval> middleware;
};

struct PathMiddleware {
    std::string prefix;
    zval middleware;
};

struct KislayPHPSession {
    bool active;
    bool previous_state;
    explicit KislayPHPSession(php_kislay_app_t *app)
        : active(false)
        , previous_state(kislay_php_thread_active) {
#if defined(ZTS)
        (void) ts_resource(0);
        ZEND_TSRMLS_CACHE_UPDATE();
        if (php_request_startup() == SUCCESS) {
            active = true;
        }
#else
        // In NTS, the request is already started by the CLI SAPI.
        // We just mark it as active to ensure we don't nest start/shutdown calls.
        active = true;
#endif
        if (active) {
            kislay_php_thread_active = true;
        }
    }

    ~KislayPHPSession() {
#if defined(ZTS)
        if (active) {
            php_request_shutdown(nullptr);
        }
#endif
        kislay_php_thread_active = previous_state;
    }

    bool is_ok() const { return active; }
};

}

// ── Task Scheduler struct ──────────────────────────────────────────────────────
struct kislay_scheduled_task {
    kislay::runtime::PhpTaskId task_id;
    enum Type { INTERVAL, ONCE, CRON } type;
    std::string cron;
    long interval_ms;
    long long next_run_ms;
    bool fired;
};

typedef struct _php_kislay_request_t {
    std::string method;
    std::string uri;
    std::string path;
    std::string query;
    std::string body;
    std::string request_id;
    std::vector<kislay::RequestField> params;
    std::vector<kislay::RequestField> query_params;
    std::vector<kislay::RequestField> body_params;
    std::string parsed_query_buffer;
    std::string parsed_body_buffer;
    kislay::runtime::FlatHeaders headers;
    std::vector<kislay::RequestAttribute> attributes;
    zval json_cache;
    bool json_cached;
    bool json_valid;
    bool query_parsed;
    bool body_parsed;
    // JWT per-request state
    bool jwt_valid;
    zval jwt_payload;
    // W3C Trace Context per-request state
    std::string trace_id;    // 32 hex chars (propagated or generated)
    std::string span_id;     // 16 hex chars (new span for this request)
    std::string traceparent; // full "00-{traceId}-{spanId}-01"
    std::string tracestate;  // forwarded tracestate header value
    const kislay::Route *matched_route{nullptr};
    zend_object std;
} php_kislay_request_t;

typedef struct _php_kislay_response_t {
    std::string body;
    zend_string *body_zstr; // Option 4: direct zend_string* to avoid copy from smart_str
    std::string file_path;
    std::string content_type;
    std::unordered_map<std::string, std::string> headers;
    bool send_file;
    zend_long status_code;
    zend_object std;
} php_kislay_response_t;

struct KislayPendingHttpTask {
    php_kislay_async_http_t *async_http;
    std::string request_id;
};

struct KislayAsyncLaneState {
    std::unique_ptr<kislay::runtime::PromiseRegistry> promise_registry;
    std::unordered_map<kislay::runtime::TaskId, KislayPendingPhpTask> pending_php_tasks;
    std::unordered_map<kislay::runtime::TaskId, KislayPendingHttpTask> pending_http_tasks;
    std::unordered_map<std::string, std::size_t> pending_request_counts;

    KislayAsyncLaneState()
        : promise_registry(new kislay::runtime::PromiseRegistry()) {}
};

typedef struct _php_kislay_app_t {
    std::vector<kislay::Route> routes;
    std::vector<zval> middleware;
    std::vector<kislay::PathMiddleware> path_middleware;
    std::vector<zval> request_start_hooks;
    std::vector<zval> request_end_hooks;
    std::vector<kislay::GroupContext> group_stack;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> exact_routes_by_method;
    std::unordered_map<std::string, std::unordered_map<std::size_t, std::vector<size_t>>> segmented_routes_by_method;
    std::unique_ptr<std::mutex> lock;
    struct mg_context *ctx;
    std::unique_ptr<kislay::runtime::UvServer> uv_server;
    std::string server_type; // "civetweb" or "libuv"
    std::atomic_bool running;
    size_t memory_limit_bytes;
    bool gc_after_request;
    int thread_count;
    int worker_count;
    zend_long read_timeout_ms;
    zend_long keep_alive_timeout_ms;
    bool enable_keep_alive;
    std::size_t queue_size;
    size_t max_body_bytes;
    bool cors_enabled;
    bool log_enabled;
    bool request_id_enabled;
    bool trace_enabled;
    bool async_enabled;
    std::string document_root;
    std::string default_tls_cert;
    std::string default_tls_key;
    std::string referrer_policy;
    bool is_zts_runtime;
    bool zts_parallel_enabled;
    std::size_t php_runtime_threads;
    std::unique_ptr<kislay::runtime::PhpRuntimePool> php_runtime_pool;
    std::atomic<std::uint64_t> next_request_task_id;

    // Async runtime
    std::atomic_bool loop_active;
    int async_worker_count;
    std::unique_ptr<kislay::runtime::AsyncBridge> async_bridge;
    std::unique_ptr<KislayAsyncLaneState> async_single_lane;
    std::vector<std::unique_ptr<KislayAsyncLaneState>> async_lanes;
    std::unordered_map<kislay::runtime::PhpTaskId, zval> scheduled_callbacks;
    std::atomic<std::uint64_t> next_async_id;

    // JWT Security
    bool jwt_enabled;
    bool jwt_required;
    std::string jwt_secret;
    std::vector<std::string> jwt_exclude_prefixes;

    // Actuator
    bool actuator_enabled;
    long long start_time_ms;
    std::uint64_t gc_request_counter;
    std::uint32_t gc_interval_requests;
    std::uint32_t max_requests; // Max requests per worker before recycling

    // Task Scheduler
    std::vector<kislay_scheduled_task> scheduled_tasks;
    std::unique_ptr<std::mutex> scheduler_lock;
    std::unique_ptr<std::thread> scheduler_thread;
    std::atomic_bool scheduler_running;

    // Sub-app mounts
    std::vector<std::pair<std::string, zval>> mounts;

    // Error middleware and lifecycle hooks
    std::vector<zval> error_handlers;
    zval not_found_handler;
    bool has_not_found_handler;
    zval unhandled_error_handler;
    bool has_unhandled_error_handler;

    // Pluggable health contributors for /actuator/health
    std::vector<zval> health_indicators;

    zend_object std;
} php_kislay_app_t;

static void kislay_async_lane_clear(KislayAsyncLaneState &lane) {
    for (auto &entry : lane.pending_php_tasks) {
        zval_ptr_dtor(&entry.second.callable);
    }
    lane.pending_php_tasks.clear();

    for (auto &entry : lane.pending_http_tasks) {
        kislay_release_async_http(entry.second.async_http);
    }
    lane.pending_http_tasks.clear();
    lane.pending_request_counts.clear();

    if (lane.promise_registry) {
        lane.promise_registry->clear();
    }
}

static inline void kislay_async_track_request(KislayAsyncLaneState *lane_state,
                                              const std::string &request_id) {
    if (lane_state == nullptr || request_id.empty()) {
        return;
    }
    auto it = lane_state->pending_request_counts.find(request_id);
    if (it == lane_state->pending_request_counts.end()) {
        lane_state->pending_request_counts.emplace(request_id, 1);
    } else {
        ++it->second;
    }
}

static inline void kislay_async_untrack_request(KislayAsyncLaneState *lane_state,
                                                const std::string &request_id) {
    if (lane_state == nullptr || request_id.empty()) {
        return;
    }
    auto it = lane_state->pending_request_counts.find(request_id);
    if (it == lane_state->pending_request_counts.end()) {
        return;
    }
    if (it->second <= 1) {
        lane_state->pending_request_counts.erase(it);
    } else {
        --it->second;
    }
}

static void kislay_async_lanes_reset(php_kislay_app_t *app, std::size_t lane_count) {
    if (app == nullptr) {
        return;
    }

    if (lane_count == 0) {
        lane_count = 1;
    }

    if (app->async_single_lane) {
        kislay_async_lane_clear(*app->async_single_lane);
    }
    for (auto &lane : app->async_lanes) {
        if (lane) {
            kislay_async_lane_clear(*lane);
        }
    }
    app->async_lanes.clear();

    if (lane_count <= 1) {
        if (!app->async_single_lane) {
            app->async_single_lane.reset(new KislayAsyncLaneState());
        }
        return;
    }

    app->async_single_lane.reset();
    app->async_lanes.reserve(lane_count);
    for (std::size_t i = 0; i < lane_count; ++i) {
        app->async_lanes.emplace_back(new KislayAsyncLaneState());
    }
}

static inline bool kislay_async_is_sharded(const php_kislay_app_t *app) {
    return app != nullptr && !app->async_lanes.empty();
}

static std::size_t kislay_async_lane_index_for_thread(const php_kislay_app_t *app) {
    if (app == nullptr || !kislay_async_is_sharded(app)) {
        return 0;
    }
    return kislay_php_runtime_lane_index < app->async_lanes.size() ? kislay_php_runtime_lane_index : 0;
}

static KislayAsyncLaneState *kislay_async_lane_state(php_kislay_app_t *app, std::size_t lane_index) {
    if (app == nullptr) {
        return nullptr;
    }
    if (!kislay_async_is_sharded(app)) {
        return app->async_single_lane.get();
    }
    if (lane_index >= app->async_lanes.size()) {
        lane_index = 0;
    }
    return app->async_lanes[lane_index].get();
}

static bool kislay_callable_requires_single_runtime_lane(const zval *callable) {
    if (callable == nullptr) {
        return false;
    }

    if (Z_TYPE_P(callable) == IS_OBJECT) {
        return true;
    }

    if (Z_TYPE_P(callable) == IS_ARRAY) {
        zval *first = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
        return first != nullptr && Z_TYPE_P(first) == IS_OBJECT;
    }

    return false;
}

static bool kislay_list_has_thread_unsafe_callbacks(const std::vector<zval> &callbacks,
                                                    const char *label,
                                                    std::string *reason_out) {
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
        if (kislay_callable_requires_single_runtime_lane(&callbacks[i])) {
            if (reason_out != nullptr) {
                *reason_out = std::string(label) + "[" + std::to_string(i) + "]";
            }
            return true;
        }
    }
    return false;
}

static bool kislay_app_has_thread_unsafe_callbacks(php_kislay_app_t *app, std::string *reason_out) {
    if (app == nullptr) {
        return false;
    }

    if (kislay_list_has_thread_unsafe_callbacks(app->middleware, "middleware", reason_out) ||
        kislay_list_has_thread_unsafe_callbacks(app->request_start_hooks, "request_start_hooks", reason_out) ||
        kislay_list_has_thread_unsafe_callbacks(app->request_end_hooks, "request_end_hooks", reason_out) ||
        kislay_list_has_thread_unsafe_callbacks(app->error_handlers, "error_handlers", reason_out)) {
        return true;
    }

    if (app->has_not_found_handler && kislay_callable_requires_single_runtime_lane(&app->not_found_handler)) {
        if (reason_out != nullptr) {
            *reason_out = "not_found_handler";
        }
        return true;
    }

    if (app->has_unhandled_error_handler &&
        kislay_callable_requires_single_runtime_lane(&app->unhandled_error_handler)) {
        if (reason_out != nullptr) {
            *reason_out = "unhandled_error_handler";
        }
        return true;
    }

    for (std::size_t i = 0; i < app->path_middleware.size(); ++i) {
        if (kislay_callable_requires_single_runtime_lane(&app->path_middleware[i].middleware)) {
            if (reason_out != nullptr) {
                *reason_out = "path_middleware[" + std::to_string(i) + "]";
            }
            return true;
        }
    }

    for (const auto &route : app->routes) {
        if (kislay_callable_requires_single_runtime_lane(&route.handler)) {
            if (reason_out != nullptr) {
                *reason_out = "route(" + route.method + " " + route.pattern + ") handler";
            }
            return true;
        }
        if (kislay_list_has_thread_unsafe_callbacks(route.middleware,
                                                    ("route(" + route.method + " " + route.pattern + ") middleware").c_str(),
                                                    reason_out) ||
            kislay_list_has_thread_unsafe_callbacks(route.compiled_middleware,
                                                    ("route(" + route.method + " " + route.pattern + ") compiled_middleware").c_str(),
                                                    reason_out)) {
            return true;
        }
    }

    return false;
}

static KislayAsyncLaneState *kislay_current_async_lane_state(php_kislay_app_t *app) {
    return kislay_async_lane_state(app, kislay_async_lane_index_for_thread(app));
}

static bool kislay_parse_long_strict(const char *value, zend_long *out) {
    if (value == nullptr || *value == '\0' || out == nullptr) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long long parsed = std::strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = static_cast<zend_long>(parsed);
    return true;
}

static bool kislay_parse_bool_text(const char *value, bool *out) {
    if (value == nullptr || *value == '\0' || out == nullptr) {
        return false;
    }
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0) {
        *out = true;
        return true;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static zend_long kislay_env_long(const char *name, zend_long fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    zend_long parsed = 0;
    if (!kislay_parse_long_strict(value, &parsed)) {
        php_error_docref(nullptr, E_WARNING,
                         "Invalid numeric value for %s=\"%s\"; using default %lld",
                         name,
                         value,
                         static_cast<long long>(fallback));
        return fallback;
    }
    return parsed;
}

static bool kislay_env_bool(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    bool parsed = fallback;
    if (!kislay_parse_bool_text(value, &parsed)) {
        php_error_docref(nullptr, E_WARNING,
                         "Invalid boolean value for %s=\"%s\"; using default %s",
                         name,
                         value,
                         fallback ? "true" : "false");
        return fallback;
    }
    return parsed;
}

static std::string kislay_env_string(const char *name, const std::string &fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::string(value);
}

static std::string kislay_ascii_lower_copy(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static bool kislay_is_supported_referrer_policy(const std::string &normalized_policy) {
    return normalized_policy == "no-referrer" ||
           normalized_policy == "no-referrer-when-downgrade" ||
           normalized_policy == "origin" ||
           normalized_policy == "origin-when-cross-origin" ||
           normalized_policy == "same-origin" ||
           normalized_policy == "strict-origin" ||
           normalized_policy == "strict-origin-when-cross-origin" ||
           normalized_policy == "unsafe-url";
}

static bool kislay_ascii_starts_with_ci(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        unsigned char lhs = static_cast<unsigned char>(value[i]);
        unsigned char rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

static std::string kislay_sanitize_referrer_policy(const std::string &candidate,
                                                   const std::string &fallback,
                                                   const char *source) {
    std::string normalized = kislay_ascii_lower_copy(candidate);
    if (normalized.empty()) {
        return fallback;
    }
    if (normalized == "off" || normalized == "none" || normalized == "false" || normalized == "0") {
        return "";
    }
    if (kislay_is_supported_referrer_policy(normalized)) {
        return normalized;
    }
    php_error_docref(nullptr, E_WARNING,
                     "%s: invalid referrer_policy=\"%s\"; using default \"%s\"",
                     source,
                     candidate.c_str(),
                     fallback.c_str());
    return fallback;
}

static zend_class_entry *kislay_app_ce;
static zend_class_entry *kislay_request_ce;
static zend_class_entry *kislay_response_ce;
static zend_class_entry *kislay_async_http_ce;
static zend_class_entry *kislay_promise_ce;
static zend_class_entry *kislay_service_client_ce;

ZEND_BEGIN_MODULE_GLOBALS(kislayphp_extension)
    zend_long http_threads;
    zend_long read_timeout_ms;
    zend_long max_body;
    zend_bool cors_enabled;
    zend_bool log_enabled;
    zend_bool async_enabled;
    zend_bool gc_enabled;
    char *document_root;
    char *tls_cert;
    char *tls_key;
    char *referrer_policy;
ZEND_END_MODULE_GLOBALS(kislayphp_extension)

ZEND_DECLARE_MODULE_GLOBALS(kislayphp_extension)

#define KISLAYPHP_EXTENSION_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(kislayphp_extension, v)

#if defined(ZTS)
ZEND_TSRMLS_CACHE_EXTERN();
#endif

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("kislayphp.http.threads", "1", PHP_INI_ALL, OnUpdateLong, http_threads, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.read_timeout_ms", "10000", PHP_INI_ALL, OnUpdateLong, read_timeout_ms, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.max_body", "0", PHP_INI_ALL, OnUpdateLong, max_body, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.cors", "0", PHP_INI_ALL, OnUpdateBool, cors_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.log", "1", PHP_INI_ALL, OnUpdateBool, log_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.async", "1", PHP_INI_ALL, OnUpdateBool, async_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.enable_gc", "1", PHP_INI_ALL, OnUpdateBool, gc_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.document_root", "", PHP_INI_ALL, OnUpdateString, document_root, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.tls_cert", "", PHP_INI_ALL, OnUpdateString, tls_cert, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.tls_key", "", PHP_INI_ALL, OnUpdateString, tls_key, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.referrer_policy", "strict-origin-when-cross-origin", PHP_INI_ALL, OnUpdateString, referrer_policy, zend_kislayphp_extension_globals, kislayphp_extension_globals)
PHP_INI_END()

static std::atomic<php_kislay_app_t *> kislay_active_app{nullptr};
// Request context must stay thread-local to avoid cross-thread pointer reuse.
static thread_local php_kislay_request_t *kislay_active_request = nullptr;
static std::atomic_bool kislay_signal_stop_requested{false};

// Global hook for other extensions (like persistence) to clean up before request shutdown
extern "C" {
    void (*kislay_request_end_observer)() = nullptr;
}

static std::atomic_bool kislay_signal_handlers_installed{false};

static int kislay_begin_request(struct mg_connection *conn);
static void kislay_install_signal_handlers();
static void kislay_promise_resolve(php_kislay_promise_t *promise, zval *value);
static void kislay_promise_reject(php_kislay_promise_t *promise, zval *reason);
static void kislay_promise_dispatch(php_kislay_promise_t *promise);
static kislay::runtime::TaskId kislay_next_async_id(php_kislay_app_t *app);
static void kislay_run_scheduler(php_kislay_app_t *obj);
static void kislay_async_drain_lane(php_kislay_app_t *app, std::size_t lane_index, std::size_t budget = 128);
static void kislay_async_drain(php_kislay_app_t *app, std::size_t budget = 128);
static bool kislay_async_has_pending_for_request(php_kislay_app_t *app, const std::string &request_id);
static void kislay_async_wait_for_request(php_kislay_app_t *app, const std::string &request_id, zend_long timeout_ms);
static bool kislay_app_start_runtime(php_kislay_app_t *app, bool require_wait_loop);
static void kislay_app_stop_runtime(php_kislay_app_t *app);
static bool kislay_app_start_server_uv(php_kislay_app_t *app, const std::string &listen_addr);
static void kislay_app_stop_server_uv(php_kislay_app_t *app);
static void kislay_process_runtime_request(std::size_t runtime_lane,
                                           php_kislay_app_t *app,
                                           kislay::runtime::RuntimeRequestMessage &request,
                                           kislay::runtime::RuntimeResponseMessage &response);

static void kislay_app_clear_active(php_kislay_app_t *app) {
    php_kislay_app_t *active = kislay_active_app.load(std::memory_order_relaxed);
    if (active == app) {
        kislay_active_app.store(nullptr, std::memory_order_relaxed);
    }
}

static void kislay_app_stop_server(php_kislay_app_t *app) {
    if (app->ctx != nullptr) {
        mg_stop(app->ctx);
        app->ctx = nullptr;
    }
    kislay_app_stop_server_uv(app);
    app->running.store(false, std::memory_order_relaxed);

    if (app->loop_active.load(std::memory_order_relaxed)) {
        app->loop_active.store(false, std::memory_order_relaxed);
        if (app->async_bridge) {
            app->async_bridge->stop();
        }
    }
    kislay_app_stop_runtime(app);

    if (app->scheduler_running.exchange(false, std::memory_order_acq_rel)) {
        if (app->scheduler_thread && app->scheduler_thread->joinable()) {
            app->scheduler_thread->join();
        }
    }

    kislay_app_clear_active(app);
    kislay_signal_stop_requested.store(false, std::memory_order_relaxed);
}

static void kislay_app_stop_runtime(php_kislay_app_t *app) {
    if (app != nullptr && app->php_runtime_pool) {
        app->php_runtime_pool->stop();
    }
}

static bool kislay_app_wait_loop(php_kislay_app_t *app, zend_long timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (app->running.load(std::memory_order_relaxed)) {
        if (app->php_runtime_pool &&
            app->php_runtime_pool->mode() == kislay::runtime::PhpRuntimeMode::SingleThreadLoop) {
            app->php_runtime_pool->drain(512);
        }
        kislay_async_drain(app, 512);
        if (kislay_signal_stop_requested.load(std::memory_order_relaxed)) {
            kislay_app_stop_server(app);
            break;
        }
        if (timeout_ms > 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }
        if (app->php_runtime_pool &&
            app->php_runtime_pool->mode() == kislay::runtime::PhpRuntimeMode::SingleThreadLoop) {
            app->php_runtime_pool->wait_for_activity_for(std::chrono::milliseconds(1));
        } else if (app->async_bridge && app->async_bridge->running()) {
            app->async_bridge->wait_for_activity_for(0, std::chrono::milliseconds(1));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

static bool kislay_app_start_runtime(php_kislay_app_t *app, bool require_wait_loop) {
    if (app == nullptr) {
        return false;
    }
    if (app->php_runtime_pool && app->php_runtime_pool->running()) {
        return true;
    }

    const bool parallel_requested = app->zts_parallel_enabled;
    bool enable_parallel = parallel_requested;
    if (enable_parallel) {
        std::string unsafe_reason;
        if (kislay_app_has_thread_unsafe_callbacks(app, &unsafe_reason)) {
            enable_parallel = false;
            if (app->log_enabled) {
                std::fprintf(stderr,
                             "[kislay] ZTS parallel runtime disabled for object-backed callback: %s\n",
                             unsafe_reason.c_str());
            }
        }
    }
    const bool dedicated_background_thread = !require_wait_loop && !enable_parallel;

    const std::size_t runtime_threads = enable_parallel
        ? static_cast<std::size_t>(std::max(app->thread_count, 1))
        : 1;

    kislay::runtime::PhpRuntimeConfig config;
    config.is_zts = app->is_zts_runtime;
    config.parallel_enabled = enable_parallel;
    config.background_thread = dedicated_background_thread;
    config.runtime_threads = runtime_threads;
    config.request_queue_size = app->queue_size;
    config.max_requests = app->max_requests;

    app->php_runtime_threads = runtime_threads;
    kislay_async_lanes_reset(app, runtime_threads);
    app->php_runtime_pool.reset(new kislay::runtime::PhpRuntimePool(
        config,
        [app](std::size_t runtime_lane,
              kislay::runtime::RuntimeRequestMessage &request,
              kislay::runtime::RuntimeResponseMessage &response) {
            kislay_process_runtime_request(runtime_lane, app, request, response);
        }
    ));

    if (app->async_enabled) {
        if (!app->async_bridge) {
            app->async_bridge.reset(new kislay::runtime::AsyncBridge());
        }
        if (!app->async_bridge->running()) {
            app->async_bridge->start(
                static_cast<std::size_t>(std::max(app->async_worker_count, 1)),
                runtime_threads
            );
        }
        app->loop_active.store(true, std::memory_order_relaxed);
    }

    app->running.store(true, std::memory_order_relaxed);
    bool ok = app->php_runtime_pool->start();
    if (ok && require_wait_loop) {
        kislay_app_wait_loop(app, -1);
    }
    return ok;
}

static void kislay_disable_stack_guard_for_nts(const char *source) {
#if !defined(ZTS)
    (void) source;
    EG(max_allowed_stack_size) = -1;
    EG(stack_limit) = nullptr;
#else
    (void) source;
#endif
}

static bool kislay_path_is_directory(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool kislay_path_is_regular_file(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

static int kislay_sanitize_thread_count(zend_long candidate, zend_long fallback, const char *source) {
    zend_long value = candidate;
    if (value <= 0) {
        php_error_docref(nullptr, E_WARNING,
                         "%s: invalid num_threads=%lld; using default %lld",
                         source,
                         static_cast<long long>(value),
                         static_cast<long long>(fallback));
        value = fallback > 0 ? fallback : 1;
    }
    return static_cast<int>(value);
}

static zend_long kislay_sanitize_timeout_ms(zend_long candidate, zend_long fallback, const char *source) {
    if (candidate >= 0) {
        return candidate;
    }
    zend_long resolved = fallback >= 0 ? fallback : 10000;
    php_error_docref(nullptr, E_WARNING,
                     "%s: invalid request_timeout_ms=%lld; using default %lld",
                     source,
                     static_cast<long long>(candidate),
                     static_cast<long long>(resolved));
    return resolved;
}

static size_t kislay_sanitize_max_body(zend_long candidate, zend_long fallback, const char *source) {
    if (candidate >= 0) {
        return static_cast<size_t>(candidate);
    }
    zend_long resolved = fallback >= 0 ? fallback : 0;
    php_error_docref(nullptr, E_WARNING,
                     "%s: invalid max_body=%lld; using default %lld",
                     source,
                     static_cast<long long>(candidate),
                     static_cast<long long>(resolved));
    return static_cast<size_t>(resolved);
}

static std::string kislay_sanitize_document_root(const std::string &candidate, const std::string &fallback, const char *source) {
    if (candidate.empty()) {
        return "";
    }
    if (kislay_path_is_directory(candidate)) {
        return candidate;
    }
    if (!fallback.empty() && kislay_path_is_directory(fallback)) {
        php_error_docref(nullptr, E_WARNING,
                         "%s: document_root=\"%s\" is not a directory; using default \"%s\"",
                         source,
                         candidate.c_str(),
                         fallback.c_str());
        return fallback;
    }
    php_error_docref(nullptr, E_WARNING,
                     "%s: document_root=\"%s\" is not a directory; disabling document root",
                     source,
                     candidate.c_str());
    return "";
}

static void kislay_sanitize_tls_paths(std::string *cert_path, std::string *key_path, const char *source) {
    if (cert_path == nullptr || key_path == nullptr) {
        return;
    }
    if (cert_path->empty() && key_path->empty()) {
        return;
    }
    if (cert_path->empty() || key_path->empty()) {
        php_error_docref(nullptr, E_WARNING,
                         "%s: TLS requires both cert and key; disabling TLS",
                         source);
        cert_path->clear();
        key_path->clear();
        return;
    }
    if (!kislay_path_is_regular_file(*cert_path) || access(cert_path->c_str(), R_OK) != 0) {
        php_error_docref(nullptr, E_WARNING,
                         "%s: TLS cert \"%s\" is not readable; disabling TLS",
                         source,
                         cert_path->c_str());
        cert_path->clear();
        key_path->clear();
        return;
    }
    if (!kislay_path_is_regular_file(*key_path) || access(key_path->c_str(), R_OK) != 0) {
        php_error_docref(nullptr, E_WARNING,
                         "%s: TLS key \"%s\" is not readable; disabling TLS",
                         source,
                         key_path->c_str());
        cert_path->clear();
        key_path->clear();
    }
}

static bool kislay_app_start_server_uv(php_kislay_app_t *app, const std::string &listen_addr) {
    std::string host = "0.0.0.0";
    int port = 8080;
    
    size_t colon = listen_addr.find(':');
    if (colon != std::string::npos) {
        host = listen_addr.substr(0, colon);
        port = std::stoi(listen_addr.substr(colon + 1));
    } else {
        port = std::stoi(listen_addr);
    }

    app->uv_server.reset(new kislay::runtime::UvServer(host, port, app->php_runtime_pool));
    return app->uv_server->start();
}

static void kislay_app_stop_server_uv(php_kislay_app_t *app) {
    if (app->uv_server) {
        app->uv_server->stop();
        app->uv_server.reset();
    }
}

static bool kislay_app_start_server(php_kislay_app_t *app,
                                    const std::string &listen_addr,
                                    const std::string &cert_path,
                                    const std::string &key_path) {
    std::vector<std::string> option_values;
    option_values.push_back("listening_ports");
    option_values.push_back(listen_addr);
    option_values.push_back("num_threads");
    option_values.push_back(std::to_string(app->thread_count));
    if (app->read_timeout_ms > 0) {
        option_values.push_back("request_timeout_ms");
        option_values.push_back(std::to_string(app->read_timeout_ms));
    }
    // Note: app->max_body_bytes is deliberately NOT forwarded to civetweb's
    // "max_request_size" option here. That option sizes civetweb's internal
    // per-connection read buffer (default 16384 bytes) used to parse the
    // request line/headers — it is not an application-level body-size limit,
    // and setting it to a small max_body_bytes value (e.g. a few hundred
    // bytes, a perfectly reasonable app-level limit) makes the buffer too
    // small to hold ordinary HTTP headers, causing mg_start() to fail and
    // the whole server to refuse to start. The actual 413 check already
    // happens independently and correctly in kislay_begin_request via
    // info->content_length > app->max_body_bytes, before any body is read.
    if (!app->document_root.empty()) {
        option_values.push_back("document_root");
        option_values.push_back(app->document_root);
    }
    if (!cert_path.empty()) {
        option_values.push_back("ssl_certificate");
        option_values.push_back(cert_path);
    }
    if (!key_path.empty()) {
        option_values.push_back("ssl_private_key");
        option_values.push_back(key_path);
    }

    // TCP_NODELAY: disable Nagle's algorithm — send small responses immediately
    option_values.push_back("tcp_nodelay");
    option_values.push_back("1");

    option_values.push_back("enable_keep_alive");
    option_values.push_back(app->enable_keep_alive ? "yes" : "no");
    if (app->enable_keep_alive && app->keep_alive_timeout_ms > 0) {
        option_values.push_back("keep_alive_timeout_ms");
        option_values.push_back(std::to_string(app->keep_alive_timeout_ms));
    }

    std::vector<const char *> options;
    for (auto &val : option_values) {
        options.push_back(val.c_str());
    }
    options.push_back(nullptr);

    struct mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = kislay_begin_request;

    app->ctx = mg_start(&callbacks, app, options.data());
    if (app->ctx == nullptr) {
        return false;
    }

    kislay_install_signal_handlers();
    kislay_active_app.store(app, std::memory_order_relaxed);
    kislay_signal_stop_requested.store(false, std::memory_order_relaxed);
    app->running.store(true, std::memory_order_relaxed);

    return true;
}

static void kislay_signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        kislay_signal_stop_requested.store(true, std::memory_order_relaxed);
        php_kislay_app_t *app = kislay_active_app.load(std::memory_order_relaxed);
        if (app != nullptr) {
            app->running.store(false, std::memory_order_relaxed);
        }
    }
}

static void kislay_install_signal_handlers() {
    bool expected = false;
    if (kislay_signal_handlers_installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::signal(SIGINT, kislay_signal_handler);
        std::signal(SIGTERM, kislay_signal_handler);
    }
}

static bool kislay_app_is_running(const php_kislay_app_t *app) {
    return app->running.load(std::memory_order_relaxed);
}

typedef struct _php_kislay_async_http_t {
    CURL *curl;
    struct curl_slist *headers;
    std::vector<std::string> header_lines;
    std::string method;
    std::string url;
    std::string request_body;
    bool use_request_body;
    std::string response_body;
    long response_code;
    long timeout_ms;
    int max_retries;
    int retry_count;
    int retry_delay_ms;
    zend_object std;
} php_kislay_async_http_t;
typedef struct _php_kislay_service_client_t {
    std::string base_url;
    std::string service_name;
    std::map<std::string, std::string> default_headers;
    long timeout_ms;
    int retry_count;
    int retry_delay_ms;
    pthread_mutex_t lock;
    zend_object std;   // MUST be last
} php_kislay_service_client_t;

static inline php_kislay_service_client_t *php_kislay_service_client_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_service_client_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_service_client_t, std));
}


static zend_object_handlers kislay_request_handlers;
static zend_object_handlers kislay_response_handlers;
static zend_object_handlers kislay_app_handlers;
static zend_object_handlers kislay_async_http_handlers;
static zend_object_handlers kislay_service_client_handlers;

static inline php_kislay_request_t *php_kislay_request_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_request_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_request_t, std));
}

static inline php_kislay_response_t *php_kislay_response_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_response_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_response_t, std));
}

static inline php_kislay_app_t *php_kislay_app_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_app_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_app_t, std));
}

static inline php_kislay_async_http_t *php_kislay_async_http_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_async_http_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_async_http_t, std));
}

static void kislay_release_async_http(php_kislay_async_http_t *async_http) {
    if (async_http != nullptr) {
        OBJ_RELEASE(&async_http->std);
    }
}

static zend_object *kislay_request_create_object(zend_class_entry *ce) {
    php_kislay_request_t *req = static_cast<php_kislay_request_t *>(
        ecalloc(1, sizeof(php_kislay_request_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&req->std, ce);
    object_properties_init(&req->std, ce);
    new (&req->method) std::string();
    new (&req->uri) std::string();
    new (&req->path) std::string();
    new (&req->query) std::string();
    new (&req->body) std::string();
    new (&req->request_id) std::string();
    new (&req->params) std::vector<kislay::RequestField>();
    new (&req->query_params) std::vector<kislay::RequestField>();
    new (&req->body_params) std::vector<kislay::RequestField>();
    new (&req->parsed_query_buffer) std::string();
    new (&req->parsed_body_buffer) std::string();
    new (&req->headers) kislay::runtime::FlatHeaders();
    new (&req->attributes) std::vector<kislay::RequestAttribute>();
    new (&req->trace_id) std::string();
    new (&req->span_id) std::string();
    new (&req->traceparent) std::string();
    new (&req->tracestate) std::string();
    // No upfront reserve — vectors start empty (capacity 0).
    // Route matching does reserve(param_names.size()) before filling params.
    // Query/body params grow on demand only when PHP code parses them.
    ZVAL_UNDEF(&req->json_cache);
    req->json_cached = false;
    req->json_valid = false;
    req->query_parsed = false;
    req->body_parsed = false;
    req->jwt_valid = false;
    ZVAL_NULL(&req->jwt_payload);
    req->std.handlers = &kislay_request_handlers;
    return &req->std;
}

static void kislay_request_free_obj(zend_object *object) {
    php_kislay_request_t *req = php_kislay_request_from_obj(object);
    for (auto &item : req->attributes) {
        zval_ptr_dtor(&item.value);
    }
    if (req->json_cached && !Z_ISUNDEF(req->json_cache)) {
        zval_ptr_dtor(&req->json_cache);
    }
    if (!Z_ISUNDEF(req->jwt_payload) && !Z_ISNULL(req->jwt_payload)) {
        zval_ptr_dtor(&req->jwt_payload);
    }
    req->attributes.~vector();
    req->headers.~FlatHeaders();
    req->parsed_body_buffer.~basic_string();
    req->parsed_query_buffer.~basic_string();
    req->body_params.~vector();
    req->query_params.~vector();
    req->params.~vector();
    req->body.~basic_string();
    req->request_id.~basic_string();
    req->query.~basic_string();
    req->path.~basic_string();
    req->uri.~basic_string();
    req->method.~basic_string();
    req->tracestate.~basic_string();
    req->traceparent.~basic_string();
    req->span_id.~basic_string();
    req->trace_id.~basic_string();
    zend_object_std_dtor(&req->std);
}

static zend_object *kislay_response_create_object(zend_class_entry *ce) {
    php_kislay_response_t *res = static_cast<php_kislay_response_t *>(
        ecalloc(1, sizeof(php_kislay_response_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&res->std, ce);
    object_properties_init(&res->std, ce);
    new (&res->body) std::string();
    new (&res->file_path) std::string();
    new (&res->content_type) std::string();
    new (&res->headers) std::unordered_map<std::string, std::string>();
    res->body_zstr = nullptr;
    res->send_file = false;
    res->status_code = 200;
    res->std.handlers = &kislay_response_handlers;
    return &res->std;
}

static void kislay_response_free_obj(zend_object *object) {
    php_kislay_response_t *res = php_kislay_response_from_obj(object);
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }
    res->headers.~unordered_map();
    res->content_type.~basic_string();
    res->file_path.~basic_string();
    res->body.~basic_string();
    zend_object_std_dtor(&res->std);
}

static zend_object *kislay_app_create_object(zend_class_entry *ce) {
    php_kislay_app_t *app = static_cast<php_kislay_app_t *>(
        ecalloc(1, sizeof(php_kislay_app_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&app->std, ce);
    object_properties_init(&app->std, ce);
    new (&app->routes) std::vector<kislay::Route>();
    new (&app->middleware) std::vector<zval>();
    new (&app->path_middleware) std::vector<kislay::PathMiddleware>();
    new (&app->request_start_hooks) std::vector<zval>();
    new (&app->request_end_hooks) std::vector<zval>();
    new (&app->group_stack) std::vector<kislay::GroupContext>();
    new (&app->exact_routes_by_method) std::unordered_map<std::string, std::unordered_map<std::string, size_t>>();
    new (&app->segmented_routes_by_method) std::unordered_map<std::string, std::unordered_map<std::size_t, std::vector<size_t>>>();
    new (&app->lock) std::unique_ptr<std::mutex>(new std::mutex());
    app->ctx = nullptr;
    new (&app->uv_server) std::unique_ptr<kislay::runtime::UvServer>();
    new (&app->server_type) std::string("civetweb");
    app->running.store(false, std::memory_order_relaxed);
    app->memory_limit_bytes = 0;
    app->gc_after_request = kislay_env_bool("KISLAYPHP_HTTP_ENABLE_GC", KISLAYPHP_EXTENSION_G(gc_enabled) != 0);
    app->thread_count = kislay_sanitize_thread_count(
        kislay_env_long("KISLAYPHP_HTTP_THREADS", KISLAYPHP_EXTENSION_G(http_threads)),
        KISLAYPHP_EXTENSION_G(http_threads),
        "Kislay\\Core\\App::__construct"
    );
    app->worker_count = kislay_env_long("KISLAYPHP_WORKERS", 1);
    if (app->worker_count < 1) app->worker_count = 1;
    app->read_timeout_ms = kislay_sanitize_timeout_ms(
        kislay_env_long("KISLAYPHP_HTTP_READ_TIMEOUT_MS", KISLAYPHP_EXTENSION_G(read_timeout_ms)),
        KISLAYPHP_EXTENSION_G(read_timeout_ms),
        "Kislay\\Core\\App::__construct"
    );
    app->enable_keep_alive = true;
    app->keep_alive_timeout_ms = 30000; // 30 s — reuse connections instead of TCP handshake per request
    app->queue_size = 2048;
    zend_long max_body = kislay_env_long("KISLAYPHP_HTTP_MAX_BODY", KISLAYPHP_EXTENSION_G(max_body));
    app->max_body_bytes = kislay_sanitize_max_body(
        max_body,
        KISLAYPHP_EXTENSION_G(max_body),
        "Kislay\\Core\\App::__construct"
    );
    app->cors_enabled = kislay_env_bool("KISLAYPHP_HTTP_CORS", KISLAYPHP_EXTENSION_G(cors_enabled) != 0);
    app->log_enabled = kislay_env_bool("KISLAYPHP_HTTP_LOG", KISLAYPHP_EXTENSION_G(log_enabled) != 0);
    app->request_id_enabled = kislay_env_bool("KISLAYPHP_HTTP_REQUEST_ID", false);
    app->trace_enabled = kislay_env_bool("KISLAYPHP_HTTP_TRACE", false);
    app->async_enabled = kislay_env_bool("KISLAYPHP_HTTP_ASYNC", KISLAYPHP_EXTENSION_G(async_enabled) != 0);
    new (&app->document_root) std::string(
        kislay_sanitize_document_root(
            kislay_env_string("KISLAYPHP_HTTP_DOCUMENT_ROOT", KISLAYPHP_EXTENSION_G(document_root) ? KISLAYPHP_EXTENSION_G(document_root) : ""),
            "",
            "Kislay\\Core\\App::__construct"
        )
    );
    new (&app->default_tls_cert) std::string(kislay_env_string("KISLAYPHP_HTTP_TLS_CERT", KISLAYPHP_EXTENSION_G(tls_cert) ? KISLAYPHP_EXTENSION_G(tls_cert) : ""));
    new (&app->default_tls_key) std::string(kislay_env_string("KISLAYPHP_HTTP_TLS_KEY", KISLAYPHP_EXTENSION_G(tls_key) ? KISLAYPHP_EXTENSION_G(tls_key) : ""));
    kislay_sanitize_tls_paths(&app->default_tls_cert, &app->default_tls_key, "Kislay\\Core\\App::__construct");
    const std::string ini_referrer_policy = KISLAYPHP_EXTENSION_G(referrer_policy) != nullptr
        ? KISLAYPHP_EXTENSION_G(referrer_policy)
        : "";
    new (&app->referrer_policy) std::string(
        kislay_sanitize_referrer_policy(
            kislay_env_string("KISLAYPHP_HTTP_REFERRER_POLICY", ini_referrer_policy),
            "",
            "Kislay\\Core\\App::__construct"
        )
    );
    app->is_zts_runtime =
#if defined(ZTS)
        true;
#else
        false;
#endif
    app->zts_parallel_enabled = app->is_zts_runtime && kislay_env_bool("KISLAY_ENABLE_ZTS_PARALLEL", false);
    app->php_runtime_threads = 1;
    new (&app->php_runtime_pool) std::unique_ptr<kislay::runtime::PhpRuntimePool>();
    app->next_request_task_id.store(1, std::memory_order_relaxed);

    app->loop_active.store(false, std::memory_order_relaxed);
    app->async_worker_count = static_cast<int>(kislay_env_long("KISLAYPHP_ASYNC_THREADS", 1));

    new (&app->async_bridge) std::unique_ptr<kislay::runtime::AsyncBridge>(new kislay::runtime::AsyncBridge());
    new (&app->async_single_lane) std::unique_ptr<KislayAsyncLaneState>(new KislayAsyncLaneState());
    new (&app->async_lanes) std::vector<std::unique_ptr<KislayAsyncLaneState>>();
    kislay_async_lanes_reset(app, 1);
    new (&app->scheduled_callbacks) std::unordered_map<kislay::runtime::PhpTaskId, zval>();
    app->next_async_id.store(1, std::memory_order_relaxed);

    // New features init
    app->jwt_enabled = false;
    app->jwt_required = false;
    new (&app->jwt_secret) std::string();
    new (&app->jwt_exclude_prefixes) std::vector<std::string>();
    app->actuator_enabled = false;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        app->start_time_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    app->gc_request_counter = 0;
    app->gc_interval_requests = 1000;
    app->max_requests = 0; // 0 means disabled
    new (&app->scheduled_tasks) std::vector<kislay_scheduled_task>();
    new (&app->scheduler_lock) std::unique_ptr<std::mutex>(new std::mutex());
    new (&app->scheduler_thread) std::unique_ptr<std::thread>();
    app->scheduler_running.store(false, std::memory_order_relaxed);
    new (&app->mounts) std::vector<std::pair<std::string, zval>>();

    new (&app->error_handlers) std::vector<zval>();
    ZVAL_UNDEF(&app->not_found_handler);
    app->has_not_found_handler = false;
    ZVAL_UNDEF(&app->unhandled_error_handler);
    app->has_unhandled_error_handler = false;
    new (&app->health_indicators) std::vector<zval>();

    app->std.handlers = &kislay_app_handlers;
    return &app->std;
}

static void kislay_app_free_obj(zend_object *object) {
    php_kislay_app_t *app = php_kislay_app_from_obj(object);
    for (auto &route : app->routes) {
        zval_ptr_dtor(&route.handler);
        for (auto &mw : route.middleware) {
            zval_ptr_dtor(&mw);
        }
        for (auto &mw : route.compiled_middleware) {
            zval_ptr_dtor(&mw);
        }
    }
    for (auto &mw : app->middleware) {
        zval_ptr_dtor(&mw);
    }
    for (auto &pmw : app->path_middleware) {
        zval_ptr_dtor(&pmw.middleware);
    }
    for (auto &hook : app->request_start_hooks) {
        zval_ptr_dtor(&hook);
    }
    for (auto &hook : app->request_end_hooks) {
        zval_ptr_dtor(&hook);
    }
    for (auto &h : app->error_handlers) { zval_ptr_dtor(&h); }
    for (auto &h : app->health_indicators) { zval_ptr_dtor(&h); }
    if (app->has_not_found_handler) { zval_ptr_dtor(&app->not_found_handler); }
    if (app->has_unhandled_error_handler) { zval_ptr_dtor(&app->unhandled_error_handler); }
    for (auto &ctx : app->group_stack) {
        for (auto &mw : ctx.middleware) {
            zval_ptr_dtor(&mw);
        }
    }
    kislay_app_stop_server(app);
    if (app->async_single_lane) {
        kislay_async_lane_clear(*app->async_single_lane);
    }
    for (auto &lane : app->async_lanes) {
        if (lane) {
            kislay_async_lane_clear(*lane);
        }
    }
    app->async_lanes.~vector();
    app->async_single_lane.~unique_ptr();
    for (auto &entry : app->scheduled_callbacks) {
        zval_ptr_dtor(&entry.second);
    }
    app->scheduled_callbacks.~unordered_map();
    app->async_bridge.~unique_ptr();
    app->php_runtime_pool.~unique_ptr();
    app->error_handlers.~vector();
    app->group_stack.~vector();
    app->request_end_hooks.~vector();
    app->request_start_hooks.~vector();
    app->path_middleware.~vector();
    app->middleware.~vector();
    app->routes.~vector();
    app->exact_routes_by_method.~unordered_map();
    app->segmented_routes_by_method.~unordered_map();
    app->document_root.~basic_string();
    app->default_tls_cert.~basic_string();
    app->default_tls_key.~basic_string();
    app->referrer_policy.~basic_string();
    app->server_type.~basic_string();
    app->uv_server.~unique_ptr();
    app->lock.~unique_ptr();
    // New features cleanup
    app->scheduler_running.store(false, std::memory_order_relaxed);
    app->scheduled_tasks.~vector();
    app->scheduler_thread.~unique_ptr();
    app->scheduler_lock.~unique_ptr();
    for (auto &m : app->mounts) {
        zval_ptr_dtor(&m.second);
    }
    app->mounts.~vector();
    app->jwt_secret.~basic_string();
    app->jwt_exclude_prefixes.~vector();
    zend_object_std_dtor(&app->std);
}

static zend_object *kislay_async_http_create_object(zend_class_entry *ce) {
    php_kislay_async_http_t *async_http = reinterpret_cast<php_kislay_async_http_t *>(
        ecalloc(1, sizeof(php_kislay_async_http_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&async_http->std, ce);
    object_properties_init(&async_http->std, ce);
    async_http->std.handlers = &kislay_async_http_handlers;
    async_http->curl = nullptr;
    async_http->headers = nullptr;
    new (&async_http->header_lines) std::vector<std::string>();
    new (&async_http->method) std::string("GET");
    new (&async_http->url) std::string();
    new (&async_http->request_body) std::string();
    async_http->use_request_body = false;
    new (&async_http->response_body) std::string();
    async_http->response_code = 0;
    async_http->timeout_ms = 10000;
    async_http->max_retries = 0;
    async_http->retry_count = 0;
    async_http->retry_delay_ms = 1000;
    return &async_http->std;
}

static void kislay_async_http_free_obj(zend_object *object) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(object);
    if (async_http->curl) {
        curl_easy_cleanup(async_http->curl);
    }
    if (async_http->headers) {
        curl_slist_free_all(async_http->headers);
    }
    async_http->header_lines.~vector();
    async_http->method.~basic_string();
    async_http->url.~basic_string();
    async_http->request_body.~basic_string();
    async_http->response_body.~basic_string();
    zend_object_std_dtor(&async_http->std);
}

static long long kislay_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ── ASCII lookup tables (no locale, branchless) ───────────────────────────────
static constexpr unsigned char KISLAY_UPPER_TABLE[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
    28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,
    53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,
    91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,
    123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,
    147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,
    166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,
    185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,
    204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,
    223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,
    242,243,244,245,246,247,248,249,250,251,252,253,254,255
};
static constexpr unsigned char KISLAY_LOWER_TABLE[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
    28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,
    53,54,55,56,57,58,59,60,61,62,63,64,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,
    117,118,119,120,121,122,
    91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,
    117,118,119,120,121,122,
    123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,
    147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,
    166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,
    185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,
    204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,
    223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,
    242,243,244,245,246,247,248,249,250,251,252,253,254,255
};

// In-place transforms — no allocation
static inline void kislay_to_upper_inplace(std::string &s) {
    for (auto &c : s) { c = static_cast<char>(KISLAY_UPPER_TABLE[static_cast<unsigned char>(c)]); }
}
static inline void kislay_to_lower_inplace(std::string &s) {
    for (auto &c : s) { c = static_cast<char>(KISLAY_LOWER_TABLE[static_cast<unsigned char>(c)]); }
}
// Keep the old signatures for callers that haven't been hot-path-optimised yet
static std::string kislay_to_upper(const std::string &value) {
    std::string out = value;
    kislay_to_upper_inplace(out);
    return out;
}
static std::string kislay_to_lower(const std::string &value) {
    std::string out = value;
    kislay_to_lower_inplace(out);
    return out;
}

static bool kislay_parse_bool_string_value(const std::string &value, bool *out) {
    if (out == nullptr) {
        return false;
    }
    std::string normalized = kislay_to_lower(value);
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        *out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        *out = false;
        return true;
    }
    return false;
}

static bool kislay_zval_to_bool(zval *value, bool fallback, const char *source) {
    if (value == nullptr) {
        return fallback;
    }
    switch (Z_TYPE_P(value)) {
        case IS_TRUE:
            return true;
        case IS_FALSE:
            return false;
        case IS_LONG:
            return Z_LVAL_P(value) != 0;
        case IS_DOUBLE:
            return Z_DVAL_P(value) != 0.0;
        case IS_STRING: {
            bool parsed = fallback;
            std::string str_value(Z_STRVAL_P(value), Z_STRLEN_P(value));
            if (kislay_parse_bool_string_value(str_value, &parsed)) {
                return parsed;
            }
            php_error_docref(nullptr, E_WARNING,
                             "%s: invalid boolean value \"%s\"; using default %s",
                             source,
                             str_value.c_str(),
                             fallback ? "true" : "false");
            return fallback;
        }
        default:
            php_error_docref(nullptr, E_WARNING,
                             "%s: invalid boolean value type; using default %s",
                             source,
                             fallback ? "true" : "false");
            return fallback;
    }
}

static bool kislay_is_valid_http_status(zend_long status) {
    return status >= 100 && status <= 599;
}

static const char *kislay_status_text(zend_long status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:
            if (status >= 200 && status < 300) return "OK";
            if (status >= 300 && status < 400) return "Redirect";
            if (status >= 400 && status < 500) return "Client Error";
            if (status >= 500 && status < 600) return "Server Error";
            return "Unknown";
    }
}

static const std::string_view *kislay_find_request_field(const std::vector<kislay::RequestField> &fields,
                                                         std::string_view key) {
    for (const auto &field : fields) {
        if (field.key == key) {
            return &field.value;
        }
    }
    return nullptr;
}

static kislay::RequestAttribute *kislay_find_request_attribute(std::vector<kislay::RequestAttribute> &attributes,
                                                               std::string_view key) {
    for (auto &attribute : attributes) {
        if (attribute.key == key) {
            return &attribute;
        }
    }
    return nullptr;
}

static const kislay::RequestAttribute *kislay_find_request_attribute(const std::vector<kislay::RequestAttribute> &attributes,
                                                                     std::string_view key) {
    for (const auto &attribute : attributes) {
        if (attribute.key == key) {
            return &attribute;
        }
    }
    return nullptr;
}

static void kislay_parse_query(std::string &buffer, std::vector<kislay::RequestField> &out) {
    out.clear();
    if (buffer.empty()) {
        return;
    }

    std::size_t pair_count = 1;
    for (char ch : buffer) {
        if (ch == '&') {
            ++pair_count;
        }
    }
    if (out.capacity() < pair_count) {
        out.reserve(pair_count);
    }

    char *base = &buffer[0];
    std::size_t start = 0;
    while (start <= buffer.size()) {
        std::size_t end = buffer.find('&', start);
        if (end == std::string::npos) {
            end = buffer.size();
        }
        if (end > start) {
            std::size_t eq = buffer.find('=', start);
            if (eq == std::string::npos || eq > end) {
                eq = end;
            }

            char *key_ptr = base + start;
            std::size_t key_len = eq - start;
            key_len = php_url_decode(key_ptr, key_len);

            char *value_ptr = base + end;
            std::size_t value_len = 0;
            if (eq < end) {
                value_ptr = base + eq + 1;
                value_len = end - eq - 1;
                value_len = php_url_decode(value_ptr, value_len);
            }

            if (key_len != 0) {
                out.push_back({
                    std::string_view(key_ptr, key_len),
                    std::string_view(value_ptr, value_len),
                });
            }
        }
        if (end == buffer.size()) {
            break;
        }
        start = end + 1;
    }
}

static std::string kislay_async_http_build_query(CURL *curl, HashTable *data) {
    std::string query;
    zend_ulong index = 0;
    zend_string *key = nullptr;
    zval *val = nullptr;

    ZEND_HASH_FOREACH_KEY_VAL(data, index, key, val) {
        std::string raw_key = key != nullptr
            ? std::string(ZSTR_VAL(key), ZSTR_LEN(key))
            : std::to_string(index);

        zend_string *raw_value = zval_get_string(val);
        char *escaped_key = curl_easy_escape(curl, raw_key.c_str(), static_cast<int>(raw_key.size()));
        char *escaped_value = curl_easy_escape(curl, ZSTR_VAL(raw_value), static_cast<int>(ZSTR_LEN(raw_value)));

        if (!query.empty()) {
            query.push_back('&');
        }
        query += escaped_key != nullptr ? escaped_key : "";
        query.push_back('=');
        query += escaped_value != nullptr ? escaped_value : "";

        if (escaped_key != nullptr) {
            curl_free(escaped_key);
        }
        if (escaped_value != nullptr) {
            curl_free(escaped_value);
        }
        zend_string_release(raw_value);
    } ZEND_HASH_FOREACH_END();

    return query;
}

static std::string kislay_async_http_append_query(const zend_string *url, const std::string &query) {
    std::string out(ZSTR_VAL(url), ZSTR_LEN(url));
    if (query.empty()) {
        return out;
    }

    if (out.find('?') == std::string::npos) {
        out.push_back('?');
    } else if (out.back() != '?' && out.back() != '&') {
        out.push_back('&');
    }
    out += query;
    return out;
}

static std::string kislay_join_paths(const std::string &prefix, const std::string &path) {
    if (prefix.empty()) {
        return path;
    }
    if (path.empty()) {
        return prefix;
    }
    if (prefix.back() == '/' && path.front() == '/') {
        return prefix + path.substr(1);
    }
    if (prefix.back() != '/' && path.front() != '/') {
        return prefix + "/" + path;
    }
    return prefix + path;
}

static std::string kislay_normalize_prefix(const std::string &prefix) {
    if (prefix.empty()) {
        return "";
    }
    std::string out = prefix;
    if (out.front() != '/') {
        out = "/" + out;
    }
    if (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

static std::string kislay_normalize_route_path(const std::string &path) {
    if (path.empty()) {
        return "/";
    }
    std::string out = path;
    if (out.front() != '/') {
        out = "/" + out;
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

static bool kislay_path_has_prefix(const std::string &path, const std::string &prefix) {
    if (prefix.empty() || prefix == "/") {
        return true;
    }
    if (path.size() < prefix.size()) {
        return false;
    }
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (path.size() == prefix.size()) {
        return true;
    }
    return path[prefix.size()] == '/';
}

static bool kislay_response_has_content(const php_kislay_response_t *res) {
    return res->send_file || !res->body.empty() || !res->headers.empty() || !res->content_type.empty() || res->status_code != 200;
}

static bool kislay_response_has_terminal_content(const php_kislay_response_t *res) {
    // Headers alone are not treated as a terminal response for unmatched routes.
    return res->send_file || !res->body.empty() || !res->content_type.empty() || res->status_code != 200;
}

static std::string kislay_now_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_snapshot;
#if defined(_WIN32)
    localtime_s(&tm_snapshot, &tt);
#else
    localtime_r(&tt, &tm_snapshot);
#endif
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm_snapshot);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lld", date_buf, static_cast<long long>(ms.count()));
    return std::string(out);
}

static std::string kislay_sanitize_log_field(const std::string &value) {
    if (value.empty()) {
        return "-";
    }
    std::string out;
    out.reserve(std::min<size_t>(value.size(), 240));
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
        if (out.size() >= 240) {
            out += "...";
            break;
        }
    }
    return out;
}

static long long kislay_response_size_bytes(const php_kislay_response_t *res) {
    if (res == nullptr) {
        return 0;
    }
    if (res->send_file) {
        auto it = res->headers.find("content-length");
        if (it != res->headers.end() && !it->second.empty()) {
            char *end = nullptr;
            long long parsed = std::strtoll(it->second.c_str(), &end, 10);
            if (end != it->second.c_str() && parsed >= 0) {
                return parsed;
            }
        }
        struct stat st;
        if (!res->file_path.empty() && stat(res->file_path.c_str(), &st) == 0 && st.st_size >= 0) {
            return static_cast<long long>(st.st_size);
        }
    }
    return static_cast<long long>(res->body.size());
}

static std::string kislay_generate_request_id() {
    unsigned char bytes[16];
    auto fill_random = [](unsigned char *target, size_t size) {
#ifdef __APPLE__
        arc4random_buf(target, size);
        return true;
#elif defined(_WIN32)
        return BCryptGenRandom(NULL, target, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
        if (RAND_bytes(target, static_cast<int>(size)) == 1) {
            return true;
        }
        int fd = ::open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            const ssize_t nread = ::read(fd, target, size);
            ::close(fd);
            if (nread == static_cast<ssize_t>(size)) {
                return true;
            }
        }
        for (size_t i = 0; i < size; ++i) {
            target[i] = static_cast<unsigned char>(rand());
        }
        return false;
#endif
    };
    fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
    return std::string(uuid);
}

static void kislay_log_request_record(const php_kislay_app_t *app,
                                      const std::string &method,
                                      const std::string &uri,
                                      zend_long status_code,
                                      long long response_bytes,
                                      long long duration_ms,
                                      const std::string &error_message) {
    if (app == nullptr || !app->log_enabled) {
        return;
    }
    const char *rid = "";
    std::string rid_str;
    if (kislay_active_request != nullptr && !kislay_active_request->request_id.empty()) {
        rid_str = kislay_active_request->request_id;
        rid = rid_str.c_str();
    }
    std::fprintf(stderr,
                 "[kislay] time=\"%s\" request_id=\"%s\" request=\"%s %s\" response=\"%lld %lldB\" duration_ms=%lld error=\"%s\"\n",
                 kislay_now_timestamp().c_str(),
                 rid,
                 method.c_str(),
                 uri.c_str(),
                 static_cast<long long>(status_code),
                 response_bytes,
                 duration_ms,
                 kislay_sanitize_log_field(error_message).c_str());
}

static void kislay_mark_internal_error(php_kislay_response_t *res) {
    if (EG(exception)) {
        zend_clear_exception();
    }
    if (!kislay_response_has_content(res)) {
        res->status_code = 500;
        res->body = "Internal Server Error";
    }
}

static bool kislay_request_is_json(php_kislay_request_t *req) {
    auto it = req->headers.find("content-type");
    if (it == req->headers.end()) {
        return false;
    }
    std::string ct = kislay_to_lower(it->second);
    return ct.find("application/json") != std::string::npos;
}

static bool kislay_request_parse_json(php_kislay_request_t *req) {
    if (req->json_cached) {
        return req->json_valid;
    }
    req->json_cached = true;
    req->json_valid = false;
    ZVAL_NULL(&req->json_cache);

    if (req->body.empty() || !kislay_request_is_json(req)) {
        return false;
    }

    zval decoded;
    ZVAL_NULL(&decoded);
    php_json_decode_ex(&decoded, req->body.c_str(), req->body.size(), PHP_JSON_OBJECT_AS_ARRAY, 512);
    if (JSON_G(error_code) != PHP_JSON_ERROR_NONE) {
        zval_ptr_dtor(&decoded);
        return false;
    }

    ZVAL_COPY_VALUE(&req->json_cache, &decoded);
    req->json_valid = true;
    return true;
}

static bool kislay_route_param_name_valid(const std::string &name) {
    if (name.empty()) {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    for (char ch : name) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            return false;
        }
    }
    return true;
}

static bool kislay_route_literal_segment_valid(std::string_view segment) {
    for (char ch : segment) {
        switch (ch) {
            case '*':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '?':
            case '+':
            case '|':
            case '^':
            case '$':
            case '\\':
                return false;
            default:
                break;
        }
    }
    return true;
}

static void kislay_collect_path_segments(std::string_view path, std::vector<std::string_view> &segments) {
    segments.clear();
    if (path.empty() || path == "/") {
        return;
    }

    std::size_t start = path.front() == '/' ? 1 : 0;
    while (start < path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (end > start) {
            segments.emplace_back(path.data() + start, end - start);
        }
        start = end + 1;
    }
}

static bool kislay_build_route(const std::string &pattern, kislay::Route *route, std::string *error_out = nullptr) {
    route->segments.clear();
    route->param_names.clear();

    std::vector<std::string_view> segments;
    kislay_collect_path_segments(pattern, segments);
    route->segments.reserve(segments.size());

    for (std::string_view segment : segments) {
        kislay::Route::Segment compiled;
        if (!segment.empty() && segment.front() == ':') {
            std::string name(segment.substr(1));
            if (!kislay_route_param_name_valid(name)) {
                if (error_out != nullptr) {
                    *error_out = "Invalid route parameter name \"" + name + "\". Use :param syntax with letters, numbers, and underscore.";
                }
                return false;
            }
            compiled.is_param = true;
            compiled.value = std::move(name);
            route->param_names.push_back(compiled.value);
        } else {
            if (!kislay_route_literal_segment_valid(segment)) {
                if (error_out != nullptr) {
                    *error_out = "Regex routes are no longer supported. Use :param syntax.";
                }
                return false;
            }
            if (segment.find(':') != std::string_view::npos) {
                if (error_out != nullptr) {
                    *error_out = "Invalid route segment \"" + std::string(segment) + "\". Parameters must use the full segment form :name.";
                }
                return false;
            }
            compiled.value.assign(segment.data(), segment.size());
        }
        route->segments.push_back(std::move(compiled));
    }

    route->exact = route->param_names.empty();
    route->param_index_map.clear();
    for (std::size_t i = 0; i < route->param_names.size(); ++i) {
        route->param_index_map[route->param_names[i]] = i;
    }
    return true;
}

static void kislay_request_ensure_query_parsed(php_kislay_request_t *req) {
    if (req->query_parsed) {
        return;
    }
    req->query_parsed = true;
    req->query_params.clear();
    if (!req->query.empty()) {
        req->parsed_query_buffer = req->query;
        kislay_parse_query(req->parsed_query_buffer, req->query_params);
    }
}

static void kislay_request_ensure_body_parsed(php_kislay_request_t *req) {
    if (req->body_parsed) {
        return;
    }
    req->body_parsed = true;
    req->body_params.clear();
    if (req->body.empty()) {
        return;
    }
    auto ct_it = req->headers.find("content-type");
    if (ct_it == req->headers.end()) {
        return;
    }
    std::string_view ct(ct_it->second);
    if (kislay_ascii_starts_with_ci(ct, "application/x-www-form-urlencoded")) {
        req->parsed_body_buffer = req->body;
        kislay_parse_query(req->parsed_body_buffer, req->body_params);
    }
}

static void kislay_request_fill_params(php_kislay_request_t *req,
                                       const kislay::Route &route,
                                       const std::vector<std::string_view> &path_segments) {
    req->params.clear();
    if (req->params.capacity() < route.param_names.size()) {
        req->params.reserve(route.param_names.size());
    }
    std::size_t param_index = 0;
    for (std::size_t i = 0; i < route.segments.size(); ++i) {
        if (!route.segments[i].is_param) {
            continue;
        }
        req->params.push_back({
            std::string_view(route.param_names[param_index++]),
            path_segments[i],
        });
    }
}

static void kislay_request_reset_state(php_kislay_request_t *req) {
    if (req == nullptr) {
        return;
    }
    req->method.clear();
    req->uri.clear();
    req->path.clear();
    req->query.clear();
    req->body.clear();
    req->request_id.clear();
    req->params.clear();
    req->query_params.clear();
    req->body_params.clear();
    req->parsed_query_buffer.clear();
    req->parsed_body_buffer.clear();
    req->headers.clear();
    for (auto &attribute : req->attributes) {
        zval_ptr_dtor(&attribute.value);
    }
    req->attributes.clear();
    if (req->json_cached && !Z_ISUNDEF(req->json_cache)) {
        zval_ptr_dtor(&req->json_cache);
    }
    ZVAL_UNDEF(&req->json_cache);
    req->json_cached = false;
    req->json_valid = false;
    if (!Z_ISUNDEF(req->jwt_payload) && !Z_ISNULL(req->jwt_payload)) {
        zval_ptr_dtor(&req->jwt_payload);
    }
    ZVAL_NULL(&req->jwt_payload);
    req->jwt_valid = false;
    req->query_parsed = false;
    req->body_parsed = false;
    req->trace_id.clear();
    req->span_id.clear();
    req->traceparent.clear();
    req->tracestate.clear();
    req->matched_route = nullptr;
}

static void kislay_response_reset_state(php_kislay_response_t *res) {
    if (res == nullptr) {
        return;
    }
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }
    res->body.clear();
    res->file_path.clear();
    res->content_type.clear();
    res->headers.clear();
    res->send_file = false;
    res->status_code = 200;
}

static void kislay_request_clone_for_subapp(php_kislay_request_t *dst,
                                            const php_kislay_request_t *src,
                                            const std::string &path_override) {
    ZEND_ASSERT(dst != nullptr);
    ZEND_ASSERT(src != nullptr);
    kislay_request_reset_state(dst);
    dst->method = src->method;
    dst->uri = src->uri;
    dst->path = path_override;
    dst->query = src->query;
    dst->body = src->body;
    dst->request_id = src->request_id;
    dst->headers = src->headers;
    dst->trace_id = src->trace_id;
    dst->span_id = src->span_id;
    dst->traceparent = src->traceparent;
    dst->tracestate = src->tracestate;
    dst->jwt_valid = src->jwt_valid;
    if (src->jwt_valid && !Z_ISNULL(src->jwt_payload)) {
        ZVAL_COPY(&dst->jwt_payload, const_cast<zval *>(&src->jwt_payload));
    }
}

static bool kislay_route_matches(const kislay::Route &route,
                                 const std::vector<std::string_view> &path_segments,
                                 php_kislay_request_t *req) {
    if (route.segments.size() != path_segments.size()) {
        return false;
    }
    for (std::size_t i = 0; i < route.segments.size(); ++i) {
        const auto &segment = route.segments[i];
        if (segment.is_param) {
            continue;
        }
        if (path_segments[i] != segment.value) {
            return false;
        }
    }
    kislay_request_fill_params(req, route, path_segments);
    return true;
}

static bool kislay_is_callable(zval *callable) {
    zend_string *callable_name = nullptr;
    bool ok = zend_is_callable(callable, 0, &callable_name) != 0;
    if (callable_name != nullptr) {
        zend_string_release(callable_name);
    }
    return ok;
}

static void kislay_response_set_body(php_kislay_response_t *res, const char *data, size_t len) {
    // Release any previously stolen zend_string before overwriting body
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }
    res->send_file = false;
    res->file_path.clear();
    res->headers.erase("content-length");
    res->body.assign(data, len);
}

static void kislay_send_marshaled_response(struct mg_connection *conn,
                                           const kislay::runtime::RuntimeResponseMessage &response,
                                           bool cors_enabled,
                                           const std::string &referrer_policy,
                                           bool emit_request_id,
                                           bool emit_trace) {
    zend_long status_code = response.status_code;
    if (!kislay_is_valid_http_status(status_code)) {
        status_code = 500;
    }
    const char *status_text = kislay_status_text(status_code);
    const bool stream_file = response.send_file && !response.file_path.empty();
    std::string content_type = response.content_type.empty() ? "text/plain" : response.content_type;
    auto header_cl = response.headers.find("content-length");
    auto header_ct = response.headers.find("content-type");
    if (header_ct != response.headers.end()) {
        content_type = header_ct->second;
    }

    // Option 1: use raw_ptr when available (zero-copy from pre-filled body buffer),
    // otherwise fall back to body.c_str().
    const char *body_ptr = response.send_raw_buffer && response.raw_ptr
        ? response.raw_ptr
        : response.body.c_str();
    const std::size_t body_len = response.send_raw_buffer && response.raw_ptr
        ? response.raw_len
        : response.body.size();

    // Option minor: stack Content-Length to avoid per-response heap alloc
    char cl_buf[24];
    snprintf(cl_buf, sizeof(cl_buf), "%zu", body_len);
    const char *content_length = cl_buf;
    std::string cl_override;

    if (stream_file && (header_cl == response.headers.end() || header_cl->second.empty())) {
        struct stat file_stat;
        if (stat(response.file_path.c_str(), &file_stat) == 0 && file_stat.st_size >= 0) {
            snprintf(cl_buf, sizeof(cl_buf), "%zu", static_cast<size_t>(file_stat.st_size));
        }
    }
    if (header_cl != response.headers.end() && !header_cl->second.empty()) {
        cl_override = header_cl->second;
        content_length = cl_override.c_str();
    }

    // Build the entire header block into one buffer and send it with a single
    // mg_write() — each mg_printf() call is its own send() syscall, and a
    // response with even a couple of extra headers previously meant 4-8+
    // separate syscalls. Same fix already applied to the Gateway extension's
    // proxy response path ("buffered proxy headers").
    thread_local std::string resp_buf;
    resp_buf.clear();
    resp_buf.reserve(256);

    resp_buf += "HTTP/1.1 ";
    resp_buf += std::to_string(static_cast<long long>(status_code));
    resp_buf += ' ';
    resp_buf += status_text;
    resp_buf += "\r\nContent-Type: ";
    resp_buf += content_type;
    resp_buf += "\r\nContent-Length: ";
    resp_buf += content_length;
    resp_buf += "\r\nConnection: keep-alive\r\n";

    if (cors_enabled) {
        resp_buf += "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Private-Network: true\r\n"
                    "Access-Control-Allow-Headers: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n";
    }
    if (!referrer_policy.empty() && response.headers.find("referrer-policy") == response.headers.end()) {
        resp_buf += "Referrer-Policy: ";
        resp_buf += referrer_policy;
        resp_buf += "\r\n";
    }
    if (emit_request_id &&
        !response.request_id.empty() &&
        response.headers.find("x-request-id") == response.headers.end()) {
        resp_buf += "X-Request-ID: ";
        resp_buf += response.request_id;
        resp_buf += "\r\n";
    }
    for (const auto &header : response.headers) {
        if (header.first == "content-type" || header.first == "content-length") {
            continue;
        }
        resp_buf += header.first;
        resp_buf += ": ";
        resp_buf += header.second;
        resp_buf += "\r\n";
    }
    if (emit_trace && !response.traceparent.empty()) {
        resp_buf += "traceparent: ";
        resp_buf += response.traceparent;
        resp_buf += "\r\n";
        if (!response.tracestate.empty()) {
            resp_buf += "tracestate: ";
            resp_buf += response.tracestate;
            resp_buf += "\r\n";
        }
    }
    resp_buf += "\r\n";
    mg_write(conn, resp_buf.data(), resp_buf.size());

    if (stream_file) {
        mg_send_file_body(conn, response.file_path.c_str());
    } else if (body_len > 0) {
        mg_write(conn, body_ptr, body_len);
    }
}

static std::string kislay_callable_debug_name(zval *callable) {
    if (callable == nullptr) {
        return "<null>";
    }
    switch (Z_TYPE_P(callable)) {
        case IS_STRING:
            return std::string(Z_STRVAL_P(callable), Z_STRLEN_P(callable));
        case IS_ARRAY: {
            zval *first = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
            zval *second = zend_hash_index_find(Z_ARRVAL_P(callable), 1);
            std::string left = "<invalid>";
            std::string right = "<invalid>";
            if (first != nullptr) {
                if (Z_TYPE_P(first) == IS_OBJECT) {
                    left = std::string(ZSTR_VAL(Z_OBJCE_P(first)->name));
                } else if (Z_TYPE_P(first) == IS_STRING) {
                    left = std::string(Z_STRVAL_P(first), Z_STRLEN_P(first));
                }
            }
            if (second != nullptr && Z_TYPE_P(second) == IS_STRING) {
                right = std::string(Z_STRVAL_P(second), Z_STRLEN_P(second));
            }
            return left + "::" + right;
        }
        case IS_OBJECT:
            return std::string(ZSTR_VAL(Z_OBJCE_P(callable)->name));
        default:
            return std::string("<type:") + zend_get_type_by_const(Z_TYPE_P(callable)) + ">";
    }
}

static std::string kislay_exception_debug_string() {
    if (EG(exception) == nullptr) {
        return "";
    }
    zend_object *exception = EG(exception);
    
    // Temporarily clear exception to safely use Zend APIs
    zend_object *old_exception = EG(exception);
    EG(exception) = nullptr;
    
    std::string class_name = exception->ce != nullptr ? std::string(ZSTR_VAL(exception->ce->name)) : "Exception";
    zval rv;
    zval *msg = zend_read_property(exception->ce, exception, ZEND_STRL("message"), 1, &rv);
    std::string out = class_name;
    if (msg != nullptr && Z_TYPE_P(msg) == IS_STRING && Z_STRLEN_P(msg) > 0) {
        out += ": " + std::string(Z_STRVAL_P(msg), ZSTR_LEN(Z_STR_P(msg)));
    }
    zval rv_file;
    zval *file = zend_read_property(exception->ce, exception, ZEND_STRL("file"), 1, &rv_file);
    zval rv_line;
    zval *line = zend_read_property(exception->ce, exception, ZEND_STRL("line"), 1, &rv_line);
    if (file != nullptr && Z_TYPE_P(file) == IS_STRING && line != nullptr && Z_TYPE_P(line) == IS_LONG) {
        out += " @ " + std::string(Z_STRVAL_P(file), ZSTR_LEN(Z_STR_P(file))) + ":" + std::to_string(static_cast<long long>(Z_LVAL_P(line)));
    }
    
    // Restore exception
    EG(exception) = old_exception;
    return out;
}

static bool kislay_call_php(zval *callable, uint32_t argc, zval *argv, zval *retval, std::string *error_out) {
    ZVAL_UNDEF(retval);
    int result = FAILURE;
    bool bailed_out = false;
    
    zend_try {
        result = call_user_function(EG(function_table), nullptr, callable, retval, argc, argv);
    } zend_catch {
        bailed_out = true;
        if (EG(exception)) {
            zend_clear_exception();
        }
    } zend_end_try();
    
    if (bailed_out) {
        if (error_out != nullptr) {
            *error_out = "engine bailout during call to " + kislay_callable_debug_name(callable);
        }
        return false;
    }
    
    if (result == FAILURE) {
        if (error_out != nullptr) {
            *error_out = "call_user_function failed for callable " + kislay_callable_debug_name(callable);
            std::string exception_text = kislay_exception_debug_string();
            if (!exception_text.empty()) {
                *error_out += " (" + exception_text + ")";
            }
        }
        return false;
    }
    if (EG(exception) != nullptr) {
        if (error_out != nullptr) {
            *error_out = "exception in callable " + kislay_callable_debug_name(callable) + " (" + kislay_exception_debug_string() + ")";
        }
        return false;
    }
    return true;
}

static bool kislay_call_static_method_one_param(zend_class_entry *ce, const char *method, zval *param, zval *retval) {
    if (ce == nullptr || method == nullptr || param == nullptr || retval == nullptr) {
        return false;
    }

    zval callable;
    array_init(&callable);
    add_next_index_str(&callable, zend_string_copy(ce->name));
    add_next_index_string(&callable, method);

    zval arg;
    ZVAL_COPY(&arg, param);
    const bool ok = call_user_function(EG(function_table), nullptr, &callable, retval, 1, &arg) == SUCCESS;
    zval_ptr_dtor(&arg);
    zval_ptr_dtor(&callable);
    return ok;
}

static void kislay_call_object_method_no_args(zval *object, const char *method) {
    if (object == nullptr || Z_TYPE_P(object) != IS_OBJECT || method == nullptr) {
        return;
    }

    zval callable;
    array_init(&callable);
    Z_TRY_ADDREF_P(object);
    add_next_index_zval(&callable, object);
    add_next_index_string(&callable, method);

    zval retval;
    ZVAL_UNDEF(&retval);
    call_user_function(EG(function_table), nullptr, &callable, &retval, 0, nullptr);
    if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
    }
    zval_ptr_dtor(&callable);
}

static bool kislay_run_middleware_list(const std::vector<zval> &list, zval *req_obj, zval *res_obj, std::string *error_out = nullptr) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(res_obj));
    for (const auto &mw : list) {
        zval args[2];
        ZVAL_COPY_VALUE(&args[0], req_obj);
        ZVAL_COPY_VALUE(&args[1], res_obj);

        zval retval;
        std::string middleware_error;
        bool ok = kislay_call_php(const_cast<zval *>(&mw), 2, args, &retval, &middleware_error);

        const bool had_exception = (EG(exception) != nullptr);
        if (!ok || had_exception) {
            if (!Z_ISUNDEF(retval)) {
                zval_ptr_dtor(&retval);
            }
            if (error_out != nullptr) {
                *error_out = middleware_error.empty() ? "middleware callback failed" : middleware_error;
            }
            if (!had_exception && !middleware_error.empty()) {
                php_error_docref(nullptr, E_WARNING, "Kislay middleware execution failed: %s", middleware_error.c_str());
            }
            kislay_mark_internal_error(res);
            return false;
        }

        bool continue_chain = true;
        if (!Z_ISUNDEF(retval)) {
            convert_to_boolean(&retval);
            continue_chain = Z_TYPE(retval) != IS_FALSE;
            zval_ptr_dtor(&retval);
        }
        if (!continue_chain) {
            if (!kislay_response_has_content(res)) {
                res->status_code = 403;
                res->body = "Middleware rejected request";
            }
            if (error_out != nullptr && error_out->empty()) {
                *error_out = "middleware rejected request";
            }
            return false;
        }
    }
    return true;
}

static bool kislay_run_hook_list(
    const std::vector<zval> &hooks,
    zval *req_obj,
    zval *res_obj,
    const char *phase,
    std::string *error_out = nullptr
) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(res_obj));
    for (const auto &hook : hooks) {
        zval args[2];
        ZVAL_COPY_VALUE(&args[0], req_obj);
        ZVAL_COPY_VALUE(&args[1], res_obj);

        zval retval;
        std::string hook_error;
        bool ok = kislay_call_php(const_cast<zval *>(&hook), 2, args, &retval, &hook_error);
        if (ok) {
            zval_ptr_dtor(&retval);
        }

        const bool had_exception = (EG(exception) != nullptr);
        if (!ok || had_exception) {
            if (error_out != nullptr) {
                *error_out = hook_error.empty() ? "request hook failed" : hook_error;
            }
            if (!had_exception) {
                php_error_docref(
                    nullptr,
                    E_WARNING,
                    "Kislay %s hook failed: %s",
                    phase,
                    hook_error.empty() ? "unknown error" : hook_error.c_str()
                );
            }
            if (!kislay_response_has_content(res)) {
                kislay_mark_internal_error(res);
            }
            return false;
        }
    }
    return true;
}

static void kislay_route_clear_compiled_middleware(kislay::Route *route) {
    for (auto &mw : route->compiled_middleware) {
        zval_ptr_dtor(&mw);
    }
    route->compiled_middleware.clear();
}

static void kislay_route_compile_middleware(php_kislay_app_t *app, kislay::Route *route) {
    kislay_route_clear_compiled_middleware(route);
    route->compiled_middleware.reserve(app->middleware.size() + app->path_middleware.size() + route->middleware.size());

    for (const auto &mw : app->middleware) {
        zval copy;
        ZVAL_COPY(&copy, const_cast<zval *>(&mw));
        route->compiled_middleware.push_back(copy);
    }
    for (const auto &pmw : app->path_middleware) {
        if (kislay_path_has_prefix(route->pattern, pmw.prefix)) {
            zval copy;
            ZVAL_COPY(&copy, const_cast<zval *>(&pmw.middleware));
            route->compiled_middleware.push_back(copy);
        }
    }
    for (const auto &mw : route->middleware) {
        zval copy;
        ZVAL_COPY(&copy, const_cast<zval *>(&mw));
        route->compiled_middleware.push_back(copy);
    }
}

static void kislay_app_rebuild_compiled_middleware(php_kislay_app_t *app) {
    for (auto &route : app->routes) {
        kislay_route_compile_middleware(app, &route);
    }
}

static kislay::Route *kislay_find_route(php_kislay_app_t *app,
                                        const std::string &method,
                                        const std::string &path,
                                        const std::vector<std::string_view> &path_segments,
                                        php_kislay_request_t *req) {
    req->matched_route = nullptr;
    
    if (app->log_enabled) {
        std::fprintf(stderr, "[kislay-debug] Finding route for Method: %s, Path: %s (Segments: %zu)\n", 
                     method.c_str(), path.c_str(), path_segments.size());
    }

    auto method_exact_it = app->exact_routes_by_method.find(method);
    if (method_exact_it != app->exact_routes_by_method.end()) {
        auto uri_exact_it = method_exact_it->second.find(path);
        if (uri_exact_it != method_exact_it->second.end() && uri_exact_it->second < app->routes.size()) {
            if (app->log_enabled) std::fprintf(stderr, "[kislay-debug] Found exact match at index %zu\n", uri_exact_it->second);
            req->params.clear();
            req->matched_route = &app->routes[uri_exact_it->second];
            return const_cast<kislay::Route *>(req->matched_route);
        }
    }

    // Fallback: HEAD can match GET
    if (method == "HEAD") {
        auto get_exact_it = app->exact_routes_by_method.find("GET");
        if (get_exact_it != app->exact_routes_by_method.end()) {
            auto uri_exact_it = get_exact_it->second.find(path);
            if (uri_exact_it != get_exact_it->second.end() && uri_exact_it->second < app->routes.size()) {
                if (app->log_enabled) std::fprintf(stderr, "[kislay-debug] Found HEAD->GET exact match at index %zu\n", uri_exact_it->second);
                req->params.clear();
                req->matched_route = &app->routes[uri_exact_it->second];
                return const_cast<kislay::Route *>(req->matched_route);
            }
        }
    }

    auto any_exact_it = app->exact_routes_by_method.find("*");
    if (any_exact_it != app->exact_routes_by_method.end()) {
        auto uri_exact_it = any_exact_it->second.find(path);
        if (uri_exact_it != any_exact_it->second.end() && uri_exact_it->second < app->routes.size()) {
            if (app->log_enabled) std::fprintf(stderr, "[kislay-debug] Found global exact match at index %zu\n", uri_exact_it->second);
            req->params.clear();
            req->matched_route = &app->routes[uri_exact_it->second];
            return const_cast<kislay::Route *>(req->matched_route);
        }
    }

    auto find_segmented = [&](const std::string &bucket_method) -> kislay::Route * {
        auto method_it = app->segmented_routes_by_method.find(bucket_method);
        if (method_it == app->segmented_routes_by_method.end()) {
            return nullptr;
        }
        auto bucket_it = method_it->second.find(path_segments.size());
        if (bucket_it == method_it->second.end()) {
            return nullptr;
        }
        for (size_t route_index : bucket_it->second) {
            if (route_index >= app->routes.size()) {
                continue;
            }
            auto &route = app->routes[route_index];
            if (kislay_route_matches(route, path_segments, req)) {
                if (app->log_enabled) std::fprintf(stderr, "[kislay-debug] Found segmented match at index %zu\n", route_index);
                req->matched_route = &route;
                return &route;
            }
        }
        return nullptr;
    };

    if (auto *route = find_segmented(method)) {
        return route;
    }
    return find_segmented("*");
}

// ─── W3C Trace Context helpers ────────────────────────────────────────────────

// Generate cryptographically random hex string of given byte length
static std::string kislay_random_hex(size_t bytes) {
    std::string result(bytes * 2, '0');
    unsigned char buf[32] = {0};
    if (bytes > sizeof(buf)) {
        bytes = sizeof(buf);
    }
#ifdef __APPLE__
    arc4random_buf(buf, bytes);
#elif defined(_WIN32)
    BCryptGenRandom(NULL, buf, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    if (RAND_bytes(buf, static_cast<int>(bytes)) != 1) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            fread(buf, 1, bytes, f);
            fclose(f);
        }
    }
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) {
        result[i*2]   = hex[buf[i] >> 4];
        result[i*2+1] = hex[buf[i] & 0xf];
    }
    return result;
}

// Parse incoming traceparent header and return trace_id, or generate a new one
static std::string kislay_parse_or_create_trace_id(const char *traceparent_hdr) {
    if (traceparent_hdr) {
        std::string hdr(traceparent_hdr);
        // format: 00-{32hex}-{16hex}-{2hex}
        std::vector<std::string> parts;
        std::stringstream ss(hdr); std::string part;
        while (std::getline(ss, part, '-')) parts.push_back(part);
        if (parts.size() >= 3 && parts[1].size() == 32) return parts[1];
    }
    return kislay_random_hex(16); // generate new 128-bit trace ID
}

// Build W3C traceparent header value for the current span
static std::string kislay_build_traceparent(const std::string &trace_id, const std::string &span_id) {
    return "00-" + trace_id + "-" + span_id + "-01";
}

static void kislay_capture_marshaled_response(php_kislay_request_t *req,
                                              php_kislay_response_t *res,
                                              kislay::runtime::RuntimeResponseMessage &response) {
    response.status_code = kislay_is_valid_http_status(res->status_code) ? res->status_code : 500;
    response.file_path = std::move(res->file_path);
    response.content_type = res->content_type.empty() ? "text/plain" : std::move(res->content_type);
    response.headers = std::move(res->headers);
    response.send_file = res->send_file;

    // Option 4: assign directly from body_zstr when available, avoiding the intermediate
    // res->body std::string allocation. body_zstr was stolen from smart_str in json()/send().
    if (res->body_zstr != nullptr) {
        response.body.assign(ZSTR_VAL(res->body_zstr), ZSTR_LEN(res->body_zstr));
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    } else {
        response.body = std::move(res->body);
    }

    // Option 1: set raw_ptr into the now-stable response.body buffer for zero-copy mg_write
    response.send_raw_buffer = !response.body.empty();
    response.refresh_raw_buffer_view();

    if (req != nullptr) {
        response.request_id = req->request_id;
        response.traceparent = req->traceparent;
        response.tracestate = req->tracestate;
    }
}

static void kislay_process_runtime_request(std::size_t runtime_lane,
                                           php_kislay_app_t *app,
                                           kislay::runtime::RuntimeRequestMessage &request,
                                           kislay::runtime::RuntimeResponseMessage &response) {
    kislay::KislayPHPSession php_session(app);
    if (!php_session.is_ok()) {
        response.status_code = 500;
        response.body = "Failed to start PHP request context";
        response.request_error = response.body;
        response.request_id = request.headers.count("x-request-id") ? request.headers.at("x-request-id") : "";
        return;
    }

    kislay_php_runtime_lane_index = runtime_lane;
    const auto request_start = UNEXPECTED(app->log_enabled)
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    zval req_obj;
    object_init_ex(&req_obj, kislay_request_ce);
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ(req_obj));
    // No kislay_request_reset_state() here — create_object already set all fields to defaults.
    req->method = std::move(request.method);
    req->uri = std::move(request.uri);
    req->path = std::move(request.route_uri);
    req->query = std::move(request.query);
    req->body = std::move(request.body);
    req->headers = std::move(request.headers);

    const bool needs_internal_request_id = app->request_id_enabled || app->async_enabled;
    auto rid_it = req->headers.find("x-request-id");
    if (UNEXPECTED(needs_internal_request_id)) {
        req->request_id = (rid_it != req->headers.end() && !rid_it->second.empty())
            ? rid_it->second
            : kislay_generate_request_id();
    } else if (rid_it != req->headers.end()) {
        req->request_id = rid_it->second;
    } else {
        req->request_id.clear();
    }

    if (UNEXPECTED(app->trace_enabled)) {
        auto tp_it = req->headers.find("traceparent");
        auto ts_it = req->headers.find("tracestate");
        const char *traceparent_hdr = tp_it != req->headers.end() ? tp_it->second.c_str() : nullptr;
        req->trace_id = kislay_parse_or_create_trace_id(traceparent_hdr);
        req->span_id = kislay_random_hex(8);
        req->traceparent = kislay_build_traceparent(req->trace_id, req->span_id);
        req->tracestate = ts_it != req->headers.end() ? ts_it->second : "";
    } else {
        req->trace_id.clear();
        req->span_id.clear();
        req->traceparent.clear();
        req->tracestate.clear();
    }

    if (app->jwt_enabled) {
        bool jwt_excluded = false;
        for (const auto &pfx : app->jwt_exclude_prefixes) {
            // compare(pos, len, other) does an in-place comparison — substr()+==
            // would allocate a temporary string on every prefix check, every
            // JWT-enabled request.
            if (req->path.size() >= pfx.size() && req->path.compare(0, pfx.size(), pfx) == 0) {
                jwt_excluded = true;
                break;
            }
        }
        if (!jwt_excluded) {
            auto auth_it = req->headers.find("authorization");
            bool jwt_ok = false;
            if (auth_it != req->headers.end()) {
                const std::string &auth_str = auth_it->second;
                if (auth_str.size() > 7 && auth_str.compare(0, 7, "Bearer ") == 0) {
                    std::string token = auth_str.substr(7);
                    auto d1 = token.find('.');
                    auto d2 = token.rfind('.');
                    if (d1 != std::string::npos && d1 != d2) {
                        std::string hdr_b64 = token.substr(0, d1);
                        std::string pay_b64 = token.substr(d1 + 1, d2 - d1 - 1);
                        std::string sig_b64 = token.substr(d2 + 1);
                        if (kislay_jwt_verify_hs256(hdr_b64, pay_b64, sig_b64, app->jwt_secret)) {
                            zval jwt_payload;
                            ZVAL_NULL(&jwt_payload);
                            if (kislay_jwt_parse_payload(pay_b64, &jwt_payload)) {
                                zval *exp_zv = Z_TYPE(jwt_payload) == IS_ARRAY
                                    ? zend_hash_str_find(Z_ARRVAL(jwt_payload), "exp", 3)
                                    : nullptr;
                                if (!exp_zv || (Z_TYPE_P(exp_zv) == IS_LONG &&
                                                Z_LVAL_P(exp_zv) >= static_cast<zend_long>(time(nullptr)))) {
                                    req->jwt_valid = true;
                                    ZVAL_COPY_VALUE(&req->jwt_payload, &jwt_payload);
                                    jwt_ok = true;
                                } else {
                                    zval_ptr_dtor(&jwt_payload);
                                }
                            }
                        }
                    }
                }
            }
            if (!jwt_ok && app->jwt_required) {
                response.status_code = 401;
                response.body = "Unauthorized";
                response.request_error = response.body;
                response.request_id = req->request_id;
                response.traceparent = req->traceparent;
                response.tracestate = req->tracestate;
                zval_ptr_dtor(&req_obj);
                return;
            }
        }
    }

    php_kislay_request_t *old_req = kislay_active_request;
    kislay_active_request = req;
    req->query_parsed = false;
    req->body_parsed = false;

    zval res_obj;
    object_init_ex(&res_obj, kislay_response_ce);
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ(res_obj));
    // No kislay_response_reset_state() here — create_object already sets body_zstr=nullptr,
    // send_file=false, status_code=200, all strings empty.

    bool handled = false;
    bool run_routes = true;
    kislay::Route *route = nullptr;
    std::string request_error;

    if (!app->mounts.empty()) {
        for (auto &mount : app->mounts) {
            if (req->path.size() >= mount.first.size() &&
                req->path.compare(0, mount.first.size(), mount.first) == 0 &&
                (req->path.size() == mount.first.size() || req->path[mount.first.size()] == '/')) {
                std::string stripped = req->path.substr(mount.first.size());
                if (stripped.empty()) {
                    stripped = "/";
                }
                zval req_obj_tmp, res_obj_tmp;
                object_init_ex(&req_obj_tmp, kislay_request_ce);
                object_init_ex(&res_obj_tmp, kislay_response_ce);
                php_kislay_request_t *sub_req = php_kislay_request_from_obj(Z_OBJ(req_obj_tmp));
                kislay_request_clone_for_subapp(sub_req, req, stripped);
                if (kislay_dispatch_to_subapp(&mount.second, request.method, stripped, &req_obj_tmp, &res_obj_tmp)) {
                    php_kislay_response_t *sub_res = php_kislay_response_from_obj(Z_OBJ(res_obj_tmp));
                    kislay_capture_marshaled_response(req, sub_res, response);
                    zval_ptr_dtor(&req_obj_tmp);
                    zval_ptr_dtor(&res_obj_tmp);
                    zval_ptr_dtor(&req_obj);
                    zval_ptr_dtor(&res_obj);
                    kislay_active_request = old_req;
                    return;
                }
                zval_ptr_dtor(&req_obj_tmp);
                zval_ptr_dtor(&res_obj_tmp);
                break;
            }
        }
    }

    if (!app->request_start_hooks.empty()) {
        run_routes = kislay_run_hook_list(
            app->request_start_hooks,
            &req_obj,
            &res_obj,
            "request-start",
            &request_error
        );
    }

    if (app->memory_limit_bytes > 0 && zend_memory_usage(0) > app->memory_limit_bytes) {
        res->status_code = 500;
        res->body = "Memory limit exceeded";
        request_error = res->body;
        run_routes = false;
        handled = true;
    }

    if (run_routes) {
        thread_local std::vector<std::string_view> path_segments;
        kislay_collect_path_segments(req->path, path_segments);
        route = kislay_find_route(app, req->method, req->path, path_segments, req);
    }
    if (run_routes && route != nullptr && !route->compiled_middleware.empty()) {
        run_routes = kislay_run_middleware_list(route->compiled_middleware, &req_obj, &res_obj, &request_error);
    }
    if (run_routes && route != nullptr) {
        const zval *route_handler = &route->handler;
        if (run_routes) {
            zval normalized_handler;
            ZVAL_UNDEF(&normalized_handler);
            if (kislay_is_controller_handler(const_cast<zval *>(route_handler))) {
                if (kislay_normalize_controller_callable(const_cast<zval *>(route_handler), &normalized_handler)) {
                    route_handler = &normalized_handler;
                }
            }
            zval args[2];
            ZVAL_COPY_VALUE(&args[0], &req_obj);
            ZVAL_COPY_VALUE(&args[1], &res_obj);
            zval retval;
            std::string handler_error;
            bool ok = kislay_call_php(const_cast<zval *>(route_handler), 2, args, &retval, &handler_error);
            if (ok) {
                zval_ptr_dtor(&retval);
            }
            if (!Z_ISUNDEF(normalized_handler)) {
                zval_ptr_dtor(&normalized_handler);
            }
            const bool had_exception = (EG(exception) != nullptr);
            if (!ok || had_exception) {
                request_error = handler_error.empty() ? "route callback failed" : handler_error;
                if (had_exception) {
                    if (!app->error_handlers.empty()) {
                        zval exception_val;
                        ZVAL_OBJ(&exception_val, EG(exception));
                        GC_ADDREF(EG(exception));
                        zend_clear_exception();
                        
                        for (auto &eh : app->error_handlers) {
                            zval eh_retval;
                            ZVAL_UNDEF(&eh_retval);
                            zval eh_args[4];
                            ZVAL_COPY_VALUE(&eh_args[0], &exception_val);
                            ZVAL_COPY_VALUE(&eh_args[1], &req_obj);
                            ZVAL_COPY_VALUE(&eh_args[2], &res_obj);
                            ZVAL_NULL(&eh_args[3]);
                            call_user_function(nullptr, nullptr, &eh, &eh_retval, 4, eh_args);
                            zval_ptr_dtor(&eh_retval);
                            if (EG(exception)) {
                                zend_clear_exception();
                            }
                        }
                        
                        zval_ptr_dtor(&exception_val);
                        if (kislay_response_has_terminal_content(res)) {
                            handled = true;
                            goto kislay_runtime_route_done;
                        }
                    } else {
                        zend_clear_exception();
                    }
                } else if (!request_error.empty()) {
                    php_error_docref(nullptr, E_WARNING, "Kislay route execution failed: %s", request_error.c_str());
                }
                kislay_mark_internal_error(res);
                run_routes = false;
            } else {
                handled = true;
            }
        }
    }
kislay_runtime_route_done:

    if (app->async_enabled && !req->request_id.empty()) {
        kislay_async_wait_for_request(app, req->request_id, app->read_timeout_ms);
        kislay_async_drain(app, 256);
    }

    if (!handled && !kislay_response_has_terminal_content(res)) {
        if (route != nullptr) {
            res->status_code = 500;
            res->body = "Internal Server Error";
            request_error = res->body;
        } else {
            const bool looks_like_browser_preflight =
                req->method == "OPTIONS" &&
                req->headers.find("origin") != req->headers.end() &&
                req->headers.find("access-control-request-method") != req->headers.end();
            if (!app->cors_enabled && looks_like_browser_preflight) {
                res->status_code = 403;
                res->body = "CORS disabled. Enable with $app->setOption('cors', true).";
            } else {
                res->status_code = 404;
                res->body = "Not Found";
            }
            request_error = res->body;
        }
    }

kislay_runtime_request_done:
    kislay_capture_marshaled_response(req, res, response);
    response.request_error = request_error;

    if (UNEXPECTED(app->log_enabled)) {
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_start
        ).count();
        kislay_log_request_record(
            app,
            req->method,
            req->uri,
            response.status_code,
            kislay_response_size_bytes(res),
            duration_ms,
            request_error
        );
    }

    if (!app->request_end_hooks.empty()) {
        kislay_run_hook_list(app->request_end_hooks, &req_obj, &res_obj, "request-end", nullptr);
    }

    zval_ptr_dtor(&req_obj);
    zval_ptr_dtor(&res_obj);
    if (UNEXPECTED(app->gc_after_request)) {
        ++app->gc_request_counter;
        if (app->gc_request_counter % app->gc_interval_requests == 0) {
            zend_gc_collect_cycles();
        }
    }

    if (kislay_request_end_observer) {
        kislay_request_end_observer();
    }

    kislay_active_request = old_req;
}

static int kislay_begin_request(struct mg_connection *conn) {
    const struct mg_request_info *info = mg_get_request_info(conn);
    if (info == nullptr || info->user_data == nullptr) {
        return 0;
    }

    auto *app = static_cast<php_kislay_app_t *>(info->user_data);
    const auto request_start = UNEXPECTED(app->log_enabled)
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    // ── Zero-alloc hot-path string processing ─────────────────────────────────
    // thread_local buffers reuse capacity across requests (assign() never
    // shrinks, so malloc only happens once per CivetWeb worker thread).
    thread_local std::string tl_method;
    thread_local std::string tl_uri;
    thread_local std::string tl_route_uri;
    thread_local std::string tl_query;
    thread_local std::string tl_hdr_name;

    // Method — CivetWeb already uppercases; in-place transform is a safety net
    tl_method.assign(info->request_method ? info->request_method : "");
    kislay_to_upper_inplace(tl_method);

    // URI — strip query string in-place, no substr allocation
    const char *raw_uri = info->local_uri ? info->local_uri
                        : (info->request_uri ? info->request_uri : "");
    tl_uri.assign(raw_uri);
    {
        const size_t qpos = tl_uri.find('?');
        if (qpos != std::string::npos) { tl_uri.resize(qpos); }
    }

    // Normalize route path in-place (add leading '/', strip trailing '/')
    tl_route_uri.assign(tl_uri);
    if (tl_route_uri.empty() || tl_route_uri.front() != '/') {
        tl_route_uri.insert(tl_route_uri.begin(), '/');
    }
    while (tl_route_uri.size() > 1 && tl_route_uri.back() == '/') {
        tl_route_uri.pop_back();
    }

    // Query string
    tl_query.assign(info->query_string ? info->query_string : "");

    // Use const-refs so the rest of the function compiles unchanged
    const std::string &method    = tl_method;
    const std::string &uri       = tl_uri;
    const std::string &route_uri = tl_route_uri;
    const std::string &log_method = method;

    if (app->cors_enabled && method == "OPTIONS") {
        thread_local std::string preflight_buf;
        preflight_buf.clear();
        preflight_buf =
            "HTTP/1.1 200 OK\r\n"
            "Allow: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Private-Network: true\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n";
        if (!app->referrer_policy.empty()) {
            preflight_buf += "Referrer-Policy: ";
            preflight_buf += app->referrer_policy;
            preflight_buf += "\r\n";
        }
        preflight_buf += "\r\n";
        mg_write(conn, preflight_buf.data(), preflight_buf.size());
        if (UNEXPECTED(app->log_enabled)) {
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - request_start
            ).count();
            kislay_log_request_record(app, log_method, uri, 200, 0, duration_ms, "");
        }
        kislay_active_request = nullptr;
        return 1;
    }

    // ── Actuator endpoints ──────────────────────────────────────────────────────
    if (app->actuator_enabled) {
        const char *act_uri = info->local_uri;
        if (act_uri && strncmp(act_uri, "/actuator/", 10) == 0 && strcmp(method.c_str(), "GET") == 0) {
            std::string apath(act_uri);
            std::string abody;
            bool act_matched = true;
            if (apath == "/actuator/health") {
                // Call each registered health indicator; aggregate status and components.
                //
                // This runs on a raw civetweb worker thread (unlike normal routes, which
                // are submitted to app->php_runtime_pool and executed on its single
                // dedicated thread on NTS builds). call_user_function/ZVAL_* below touch
                // Zend's memory manager directly, which has no per-thread isolation on
                // NTS - concurrent calls from multiple worker threads corrupt the shared
                // Zend heap and abort the whole process (zend_mm_panic), the same crash
                // class found and fixed in the Gateway extension's proxy path. A static
                // mutex serializes this rare, non-hot-path block so only one thread is
                // ever inside Zend at a time from here.
                static std::mutex actuator_health_zend_lock;
                std::lock_guard<std::mutex> actuator_health_guard(actuator_health_zend_lock);
                std::string overall = "UP";
                std::string components = "";
                bool first_comp = true;
                for (auto &indicator : app->health_indicators) {
                    zval retval;
                    ZVAL_UNDEF(&retval);
                    zval ind_copy;
                    ZVAL_COPY(&ind_copy, &indicator);
                    int call_ok = call_user_function(CG(function_table), nullptr, &ind_copy,
                                                     &retval, 0, nullptr);
                    zval_ptr_dtor(&ind_copy);
                    if (call_ok == SUCCESS && !EG(exception) && Z_TYPE(retval) == IS_ARRAY) {
                        std::string comp_name = "unknown";
                        std::string comp_status = "UP";
                        zval *name_zv = zend_hash_str_find(Z_ARRVAL(retval), "name", 4);
                        zval *stat_zv = zend_hash_str_find(Z_ARRVAL(retval), "status", 6);
                        if (name_zv && Z_TYPE_P(name_zv) == IS_STRING) {
                            comp_name = std::string(Z_STRVAL_P(name_zv), Z_STRLEN_P(name_zv));
                        }
                        if (stat_zv && Z_TYPE_P(stat_zv) == IS_STRING) {
                            comp_status = std::string(Z_STRVAL_P(stat_zv), Z_STRLEN_P(stat_zv));
                        }
                        if (comp_status != "UP") overall = "DOWN";
                        if (!first_comp) components += ",";
                        first_comp = false;
                        components += "\"" + comp_name + "\":{\"status\":\"" + comp_status + "\"";
                        zval *det_zv = zend_hash_str_find(Z_ARRVAL(retval), "details", 7);
                        if (det_zv && Z_TYPE_P(det_zv) == IS_ARRAY) {
                            components += ",\"details\":{";
                            bool first_d = true;
                            zend_string *dk; zval *dv;
                            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(det_zv), dk, dv) {
                                if (!dk) continue;
                                if (!first_d) components += ","; first_d = false;
                                components += "\"" + std::string(ZSTR_VAL(dk), ZSTR_LEN(dk)) + "\":";
                                if (Z_TYPE_P(dv) == IS_STRING) components += "\"" + std::string(Z_STRVAL_P(dv), Z_STRLEN_P(dv)) + "\"";
                                else if (Z_TYPE_P(dv) == IS_LONG)  components += std::to_string(Z_LVAL_P(dv));
                                else if (Z_TYPE_P(dv) == IS_DOUBLE) components += std::to_string(Z_DVAL_P(dv));
                                else if (Z_TYPE_P(dv) == IS_TRUE)  components += "true";
                                else if (Z_TYPE_P(dv) == IS_FALSE) components += "false";
                                else components += "null";
                            } ZEND_HASH_FOREACH_END();
                            components += "}";
                        }
                        components += "}";
                        zval_ptr_dtor(&retval);
                    } else {
                        overall = "DOWN";
                        if (EG(exception)) zend_clear_exception();
                        if (!Z_ISUNDEF(retval)) zval_ptr_dtor(&retval);
                    }
                }
                abody = "{\"status\":\"" + overall + "\",\"uptime_ms\":" +
                    std::to_string(kislay_now_ms() - app->start_time_ms);
                if (!components.empty()) abody += ",\"components\":{" + components + "}";
                abody += "}";
            } else if (apath == "/actuator/ping") {
                abody = "\"pong\"";
            } else if (apath == "/actuator/info") {
                abody = std::string("{\"php\":\"") + PHP_VERSION + "\",\"extension\":\"kislayphp\"}";
            } else if (apath == "/actuator/routes") {
                abody = "["; bool afirst = true;
                for (auto &r : app->routes) {
                    if (!afirst) abody += ","; afirst = false;
                    abody += std::string("{\"method\":\"") + r.method + "\",\"path\":\"" + r.pattern + "\"}";
                }
                abody += "]";
            } else if (apath == "/actuator/metrics") {
                abody = "{}";
            } else {
                act_matched = false;
            }
            if (act_matched) {
                mg_send_http_ok(conn, "application/json", abody.size());
                mg_write(conn, abody.data(), abody.size());
                kislay_active_request = nullptr;
                return 200;
            }
        }
    }

    if (app->max_body_bytes > 0 && info->content_length > static_cast<long long>(app->max_body_bytes)) {
        thread_local std::string too_large_buf;
        too_large_buf.clear();
        too_large_buf =
            "HTTP/1.1 413 Payload Too Large\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 17\r\n"
            "Connection: keep-alive\r\n";
        if (app->cors_enabled) {
            too_large_buf +=
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Private-Network: true\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n";
        }
        if (!app->referrer_policy.empty()) {
            too_large_buf += "Referrer-Policy: ";
            too_large_buf += app->referrer_policy;
            too_large_buf += "\r\n";
        }
        too_large_buf += "\r\nPayload Too Large";
        mg_write(conn, too_large_buf.data(), too_large_buf.size());
        if (UNEXPECTED(app->log_enabled)) {
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - request_start
            ).count();
            std::fprintf(stderr,
                         "[kislay] time=\"%s\" request_id=\"\" request=\"%s %s\" response=\"413 17B\" duration_ms=%lld error=\"Payload Too Large\"\n",
                         kislay_now_timestamp().c_str(),
                         log_method.c_str(),
                         uri.c_str(),
                         static_cast<long long>(duration_ms));
        }
        return 1;
    }

    std::string body;
    if (info->content_length > 0) {
        body.resize(static_cast<size_t>(info->content_length));
        size_t received = 0;
        while (received < body.size()) {
            int read_len = mg_read(conn, &body[received], body.size() - received);
            if (read_len <= 0) {
                break;
            }
            received += static_cast<size_t>(read_len);
        }
        body.resize(received);
    }

    kislay::runtime::RuntimeRequestMessage request;
    request.task_id = app->next_request_task_id.fetch_add(1, std::memory_order_relaxed);
    request.method    = method;
    request.uri       = uri;
    request.route_uri = route_uri;
    request.query     = tl_query;
    request.body      = std::move(body);
    request.headers.reserve(static_cast<std::size_t>(std::max(info->num_headers, 0)) + 3);
    for (int i = 0; i < info->num_headers; ++i) {
        if (info->http_headers[i].name && info->http_headers[i].value) {
            // In-place lower-case reuses tl_hdr_name's capacity — no malloc per header
            tl_hdr_name.assign(info->http_headers[i].name);
            kislay_to_lower_inplace(tl_hdr_name);
            request.headers[tl_hdr_name] = info->http_headers[i].value;
        }
    }
    const bool needs_internal_request_id = app->request_id_enabled || app->async_enabled;
    if (UNEXPECTED(needs_internal_request_id)) {
        auto correlation_it = request.headers.find("x-correlation-id");
        auto request_id_it = request.headers.find("x-request-id");

        if (correlation_it == request.headers.end() || correlation_it->second.empty() ||
            request_id_it == request.headers.end() || request_id_it->second.empty()) {
            const std::string resolved_request_id =
                (request_id_it != request.headers.end() && !request_id_it->second.empty())
                    ? request_id_it->second
                    : ((correlation_it != request.headers.end() && !correlation_it->second.empty())
                        ? correlation_it->second
                        : kislay_generate_request_id());

            if (request_id_it == request.headers.end() || request_id_it->second.empty()) {
                request.headers["x-request-id"] = resolved_request_id;
            }
            if (correlation_it == request.headers.end() || correlation_it->second.empty()) {
                request.headers["x-correlation-id"] = resolved_request_id;
            }
        }
    }
    if (app->php_runtime_pool == nullptr || !app->php_runtime_pool->running()) {
        mg_send_http_error(conn, 503, "PHP runtime is not running");
        return 503;
    }

    // Thread-local completion: reused across requests on this CivetWeb thread,
    // eliminating one heap allocation per request.  Safe because wait_for() blocks
    // until complete() returns, so the object is exclusively owned by this thread
    // for the entire request lifetime.  The no-op deleter prevents double-free.
    thread_local kislay::runtime::RequestCompletion tl_completion;
    tl_completion.reset();
    request.completion = std::shared_ptr<kislay::runtime::RequestCompletion>(
        &tl_completion, [](kislay::runtime::RequestCompletion *) {});

    if (!app->php_runtime_pool->submit(std::move(request))) {
        mg_send_http_error(conn, 503, "PHP runtime queue is overloaded");
        return 503;
    }

    kislay::runtime::RuntimeResponseMessage response;
    const auto timeout = std::chrono::milliseconds(app->read_timeout_ms >= 0 ? app->read_timeout_ms : 10000);
    if (!tl_completion.wait_for(response, timeout)) {
        mg_send_http_error(conn, 504, "PHP runtime timed out");
        return 504;
    }

    kislay_send_marshaled_response(conn,
                                   response,
                                   app->cors_enabled,
                                   app->referrer_policy,
                                   app->request_id_enabled,
                                   app->trace_enabled);
    if (app->log_enabled) {
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_start
        ).count();
        std::fprintf(stderr,
                     "[kislay] time=\"%s\" request_id=\"%s\" request=\"%s %s\" response=\"%lld %lldB\" duration_ms=%lld error=\"%s\"\n",
                     kislay_now_timestamp().c_str(),
                     response.request_id.c_str(),
                     log_method.c_str(),
                     uri.c_str(),
                     static_cast<long long>(response.status_code),
                     static_cast<long long>(response.send_file ? 0 : response.body.size()),
                     duration_ms,
                     kislay_sanitize_log_field(response.request_error).c_str());
    }
    return 1;
}

// ── New feature arginfos ──────────────────────────────────────────────────────
ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_every, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, interval_ms, IS_LONG, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_once, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, delay_ms, IS_LONG, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_schedule, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, cron, IS_STRING, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_mount, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, prefix, IS_STRING, 0)
    ZEND_ARG_INFO(0, subApp)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_req_has_role, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, role, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_get, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_json, 0, 0, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_set_body, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_set_status, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_set_header, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_set_header_alias, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_status, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_send, 0, 0, 1)
    ZEND_ARG_INFO(0, body)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_type, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, contentType, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_send_json, 0, 0, 1)
    ZEND_ARG_INFO(0, data)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_send_xml, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, xml, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_response_send_file, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, filePath, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, contentType, IS_STRING, 1)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislay_req_id, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_get_header, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_header, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_query, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_input, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_only, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, keys, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_has, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_get_attribute, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, default)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_request_set_attribute, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_req_trace_id, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_req_span_id, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_req_traceparent, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_req_tracestate, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_use, 0, 0, 1)
    ZEND_ARG_INFO(0, pathOrMiddleware)
    ZEND_ARG_CALLABLE_INFO(0, middleware, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_route, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_CALLABLE_INFO(0, handler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_wait, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, timeoutMs, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_listen, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_ARRAY_INFO(0, tls, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_group, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, prefix, IS_STRING, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
    ZEND_ARG_ARRAY_INFO(0, middleware, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_request_hook, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, hook, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_on_not_found, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, handler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_set_memory_limit, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, bytes, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_get_memory_limit, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_enable_gc, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_async_http_request, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
    ZEND_ARG_ARRAY_INFO(0, data, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_async_http_set_header, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_async_http_retry, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, max_retries, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, delay_ms, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_enable_async, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_set_option, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_async, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, task, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_promise_then, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, onFulfilled, 0)
    ZEND_ARG_CALLABLE_INFO(0, onRejected, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_promise_catch, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, onRejected, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_promise_finally, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, onFinally, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(KislayRequest, getMethod) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->method.c_str());
}

PHP_METHOD(KislayRequest, getUri) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->uri.c_str());
}

PHP_METHOD(KislayRequest, getBody) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->body.c_str());
}

PHP_METHOD(KislayRequest, body) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->body.c_str());
}

PHP_METHOD(KislayRequest, getParams) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    array_init(return_value);
    for (const auto &item : req->params) {
        add_assoc_stringl_ex(return_value,
                             item.key.data(),
                             item.key.size(),
                             item.value.data(),
                             item.value.size());
    }
}

PHP_METHOD(KislayRequest, param) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string_view key(name, name_len);
    
    // O(1) lookup if matched_route is available
    if (req->matched_route != nullptr) {
        auto it = req->matched_route->param_index_map.find(std::string(key));
        if (it != req->matched_route->param_index_map.end() && it->second < req->params.size()) {
            const auto &val = req->params[it->second].value;
            RETURN_STRINGL(val.data(), val.size());
        }
    }

    // Fallback to O(N) search
    const std::string_view *value = kislay_find_request_field(req->params, key);
    if (value == nullptr) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_STRINGL(value->data(), value->size());
}

PHP_METHOD(KislayRequest, method) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->method.c_str());
}

PHP_METHOD(KislayRequest, path) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->path.c_str());
}

PHP_METHOD(KislayRequest, getPath) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->path.c_str());
}

PHP_METHOD(KislayRequest, getQuery) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->query.c_str());
}

PHP_METHOD(KislayRequest, query) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_query_parsed(req);
    std::string_view key(name, name_len);
    const std::string_view *value = kislay_find_request_field(req->query_params, key);
    if (value == nullptr) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_STRINGL(value->data(), value->size());
}

PHP_METHOD(KislayRequest, getQueryParams) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_query_parsed(req);
    array_init(return_value);
    for (const auto &item : req->query_params) {
        add_assoc_stringl_ex(return_value,
                             item.key.data(),
                             item.key.size(),
                             item.value.data(),
                             item.value.size());
    }
}

PHP_METHOD(KislayRequest, getHeaders) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    array_init(return_value);
    for (const auto &item : req->headers) {
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
    }
}

PHP_METHOD(KislayRequest, getHeader) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    auto it = req->headers.find(key);
    if (it == req->headers.end()) {
        RETURN_NULL();
    }
    RETURN_STRING(it->second.c_str());
}

PHP_METHOD(KislayRequest, header) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    auto it = req->headers.find(key);
    if (it == req->headers.end()) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_STRING(it->second.c_str());
}

PHP_METHOD(KislayRequest, hasHeader) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    RETURN_BOOL(req->headers.find(key) != req->headers.end());
}

PHP_METHOD(KislayRequest, input) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_body_parsed(req);
    kislay_request_ensure_query_parsed(req);
    std::string_view key(name, name_len);
    if (const std::string_view *param_value = kislay_find_request_field(req->params, key)) {
        RETURN_STRINGL(param_value->data(), param_value->size());
    }
    if (const std::string_view *body_value = kislay_find_request_field(req->body_params, key)) {
        RETURN_STRINGL(body_value->data(), body_value->size());
    }
    if (const std::string_view *query_value = kislay_find_request_field(req->query_params, key)) {
        RETURN_STRINGL(query_value->data(), query_value->size());
    }
    if (default_val != nullptr) {
        RETURN_ZVAL(default_val, 1, 0);
    }
    RETURN_NULL();
}

PHP_METHOD(KislayRequest, json) {
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_request_parse_json(req)) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_ZVAL(&req->json_cache, 1, 0);
}

PHP_METHOD(KislayRequest, getJson) {
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_request_parse_json(req)) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_ZVAL(&req->json_cache, 1, 0);
}

PHP_METHOD(KislayRequest, isJson) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_BOOL(kislay_request_is_json(req) ? 1 : 0);
}

PHP_METHOD(KislayRequest, all) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_body_parsed(req);
    kislay_request_ensure_query_parsed(req);
    array_init(return_value);

    for (const auto &item : req->query_params) {
        add_assoc_stringl_ex(return_value,
                             item.key.data(),
                             item.key.size(),
                             item.value.data(),
                             item.value.size());
    }
    for (const auto &item : req->body_params) {
        add_assoc_stringl_ex(return_value,
                             item.key.data(),
                             item.key.size(),
                             item.value.data(),
                             item.value.size());
    }
    for (const auto &item : req->params) {
        add_assoc_stringl_ex(return_value,
                             item.key.data(),
                             item.key.size(),
                             item.value.data(),
                             item.value.size());
    }
}

PHP_METHOD(KislayRequest, has) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_body_parsed(req);
    kislay_request_ensure_query_parsed(req);
    std::string_view key(name, name_len);
    if (kislay_find_request_field(req->params, key) != nullptr) {
        RETURN_TRUE;
    }
    if (kislay_find_request_field(req->body_params, key) != nullptr) {
        RETURN_TRUE;
    }
    if (kislay_find_request_field(req->query_params, key) != nullptr) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

PHP_METHOD(KislayRequest, only) {
    zval *keys = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(keys)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    kislay_request_ensure_body_parsed(req);
    kislay_request_ensure_query_parsed(req);
    array_init(return_value);

    HashTable *ht = Z_ARRVAL_P(keys);
    zval *entry = nullptr;
    ZEND_HASH_FOREACH_VAL(ht, entry) {
        zend_string *key_str = zval_get_string(entry);
        std::string_view key(ZSTR_VAL(key_str), ZSTR_LEN(key_str));

        if (const std::string_view *param_value = kislay_find_request_field(req->params, key)) {
            add_assoc_stringl_ex(return_value, key.data(), key.size(), param_value->data(), param_value->size());
            zend_string_release(key_str);
            continue;
        }
        if (const std::string_view *body_value = kislay_find_request_field(req->body_params, key)) {
            add_assoc_stringl_ex(return_value, key.data(), key.size(), body_value->data(), body_value->size());
            zend_string_release(key_str);
            continue;
        }
        if (const std::string_view *query_value = kislay_find_request_field(req->query_params, key)) {
            add_assoc_stringl_ex(return_value, key.data(), key.size(), query_value->data(), query_value->size());
            zend_string_release(key_str);
            continue;
        }
        zend_string_release(key_str);
    } ZEND_HASH_FOREACH_END();
}

PHP_METHOD(KislayRequest, setAttribute) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *value = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key(name, name_len);
    if (auto *attribute = kislay_find_request_attribute(req->attributes, key)) {
        zval_ptr_dtor(&attribute->value);
        attribute->key = std::move(key);
        ZVAL_COPY(&attribute->value, value);
        RETURN_TRUE;
    }
    kislay::RequestAttribute attribute;
    attribute.key = std::move(key);
    ZVAL_COPY(&attribute.value, value);
    req->attributes.push_back(std::move(attribute));
    RETURN_TRUE;
}

PHP_METHOD(KislayRequest, getAttribute) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *default_val = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_val)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key(name, name_len);
    const auto *attribute = kislay_find_request_attribute(req->attributes, key);
    if (attribute == nullptr) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_ZVAL(const_cast<zval *>(&attribute->value), 1, 0);
}

PHP_METHOD(KislayRequest, hasAttribute) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key(name, name_len);
    RETURN_BOOL(kislay_find_request_attribute(req->attributes, key) != nullptr);
}

PHP_METHOD(KislayRequest, id) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->request_id.c_str());
}

PHP_METHOD(KislayResponse, setBody) {
    char *body = nullptr;
    size_t body_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(body, body_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    kislay_response_set_body(res, body, body_len);
}

PHP_METHOD(KislayResponse, setStatusCode) {
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = status;
}

PHP_METHOD(KislayResponse, getBody) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(res->body.c_str());
}

PHP_METHOD(KislayResponse, getStatusCode) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    RETURN_LONG(res->status_code);
}

PHP_METHOD(KislayResponse, setHeader) {
    char *name = nullptr;
    size_t name_len = 0;
    char *value = nullptr;
    size_t value_len = 0;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_STRING(value, value_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    std::string val(value, value_len);
    if (key == "content-type") {
        res->content_type = val;
    }
    res->headers[key] = val;
    RETURN_TRUE;
}

PHP_METHOD(KislayResponse, header) {
    char *name = nullptr;
    size_t name_len = 0;
    char *value = nullptr;
    size_t value_len = 0;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_STRING(value, value_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    std::string val(value, value_len);
    if (key == "content-type") {
        res->content_type = val;
    }
    res->headers[key] = val;
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, set) {
    char *name = nullptr;
    size_t name_len = 0;
    char *value = nullptr;
    size_t value_len = 0;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_STRING(value, value_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    std::string key = kislay_to_lower(std::string(name, name_len));
    std::string val(value, value_len);
    if (key == "content-type") {
        res->content_type = val;
    }
    res->headers[key] = val;
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, type) {
    char *value = nullptr;
    size_t value_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(value, value_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->content_type.assign(value, value_len);
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, status) {
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = status;
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, sendStatus) {
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = status;
    const char *text = kislay_status_text(status);
    kislay_response_set_body(res, text, std::strlen(text));
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, send) {
    zval *body = nullptr;
    zend_long status = 0;
    bool has_status = false;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(body)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (ZEND_NUM_ARGS() == 2) {
        has_status = true;
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    if (has_status) {
        if (!kislay_is_valid_http_status(status)) {
            zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
            RETURN_NULL();
        }
        res->status_code = status;
    }

    // Release any existing body_zstr
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }

    // Option 4: for string input, store zend_string* directly (zval_get_string adds a refcount,
    // no copy needed until marshal time)
    zend_string *body_str = zval_get_string(body);
    res->body.clear();
    res->headers.erase("content-length");
    res->send_file = false;
    res->file_path.clear();
    res->body_zstr = body_str; // takes ownership of the refcount from zval_get_string
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, sendJson) {
    zval *data = nullptr;
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    // Release any existing body_zstr before encoding
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    // Option 4: steal the smart_str zend_string directly — no copy into res->body
    if (buf.s != nullptr) {
        res->body.clear();
        res->headers.erase("content-length");
        res->send_file = false;
        res->file_path.clear();
        res->body_zstr = buf.s; // steal — do NOT call smart_str_free
        buf.s = nullptr;
    } else {
        kislay_response_set_body(res, "", 0);
    }

    res->status_code = status;
    res->content_type = "application/json; charset=utf-8";
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, sendXml) {
    char *xml = nullptr;
    size_t xml_len = 0;
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(xml, xml_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }
    kislay_response_set_body(res, xml, xml_len);
    res->status_code = status;
    res->content_type = "application/xml; charset=utf-8";
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, json) {
    zval *data = nullptr;
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    // Release any existing body_zstr before encoding
    if (res->body_zstr != nullptr) {
        zend_string_release(res->body_zstr);
        res->body_zstr = nullptr;
    }
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    // Option 4: steal the smart_str zend_string directly — no copy into res->body
    if (buf.s != nullptr) {
        res->body.clear();
        res->headers.erase("content-length");
        res->send_file = false;
        res->file_path.clear();
        res->body_zstr = buf.s; // steal — do NOT call smart_str_free
        buf.s = nullptr;
    } else {
        kislay_response_set_body(res, "", 0);
    }

    res->status_code = status;
    res->content_type = "application/json; charset=utf-8";
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, xml) {
    char *xml = nullptr;
    size_t xml_len = 0;
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(xml, xml_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }
    kislay_response_set_body(res, xml, xml_len);
    res->status_code = status;
    res->content_type = "application/xml; charset=utf-8";
    RETURN_ZVAL(getThis(), 1, 0);
}

static void kislay_response_apply_optional_payload(php_kislay_response_t *res, zval *payload, const char *fallback_text) {
    if (payload == nullptr || Z_TYPE_P(payload) == IS_NULL) {
        if (fallback_text != nullptr) {
            kislay_response_set_body(res, fallback_text, std::strlen(fallback_text));
            res->content_type = "text/plain; charset=utf-8";
        } else {
            kislay_response_set_body(res, "", 0);
            res->content_type.clear();
        }
        return;
    }

    if (Z_TYPE_P(payload) == IS_ARRAY || Z_TYPE_P(payload) == IS_OBJECT) {
        smart_str buf = {0};
        php_json_encode(&buf, payload, 0);
        smart_str_0(&buf);
        if (buf.s != nullptr) {
            kislay_response_set_body(res, ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
        } else {
            kislay_response_set_body(res, "", 0);
        }
        smart_str_free(&buf);
        res->content_type = "application/json; charset=utf-8";
        return;
    }

    zend_string *msg = zval_get_string(payload);
    kislay_response_set_body(res, ZSTR_VAL(msg), ZSTR_LEN(msg));
    zend_string_release(msg);
    res->content_type = "text/plain; charset=utf-8";
}

PHP_METHOD(KislayResponse, ok) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 200;
    kislay_response_apply_optional_payload(res, payload, nullptr);
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, created) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 201;
    kislay_response_apply_optional_payload(res, payload, nullptr);
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, noContent) {
    ZEND_PARSE_PARAMETERS_NONE();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 204;
    kislay_response_set_body(res, "", 0);
    res->content_type.clear();
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, badRequest) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 400;
    kislay_response_apply_optional_payload(res, payload, "Bad Request");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, unauthorized) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 401;
    kislay_response_apply_optional_payload(res, payload, "Unauthorized");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, forbidden) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 403;
    kislay_response_apply_optional_payload(res, payload, "Forbidden");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, notFound) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 404;
    kislay_response_apply_optional_payload(res, payload, "Not Found");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, methodNotAllowed) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 405;
    kislay_response_apply_optional_payload(res, payload, "Method Not Allowed");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, conflict) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 409;
    kislay_response_apply_optional_payload(res, payload, "Conflict");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, unprocessableEntity) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 422;
    kislay_response_apply_optional_payload(res, payload, "Unprocessable Entity");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, internalServerError) {
    zval *payload = nullptr;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(payload)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = 500;
    kislay_response_apply_optional_payload(res, payload, "Internal Server Error");
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayResponse, sendFile) {
    char *file_path = nullptr;
    size_t file_path_len = 0;
    char *content_type = nullptr;
    size_t content_type_len = 0;
    zend_long status = 200;
    bool has_content_type = false;

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STRING(file_path, file_path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(content_type, content_type_len)
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (ZEND_NUM_ARGS() >= 2) {
        has_content_type = true;
    }

    if (!kislay_is_valid_http_status(status)) {
        zend_throw_exception(zend_ce_exception, "HTTP status code must be between 100 and 599", 0);
        RETURN_NULL();
    }

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));

    // Check if file exists
    if (access(file_path, F_OK) != 0) {
        zend_throw_error(NULL, "File not found: %s", file_path);
        RETURN_NULL();
    }

    // Check if file is readable
    if (access(file_path, R_OK) != 0) {
        zend_throw_error(NULL, "File not readable: %s", file_path);
        RETURN_NULL();
    }

    // Get file size
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        zend_throw_error(NULL, "Could not get file information: %s", file_path);
        RETURN_NULL();
    }

    if (file_stat.st_size < 0) {
        zend_throw_error(NULL, "Invalid file size: %s", file_path);
        RETURN_NULL();
    }

    size_t file_size = static_cast<size_t>(file_stat.st_size);

    // Set response
    res->send_file = true;
    res->file_path.assign(file_path, file_path_len);
    res->body.clear();
    res->status_code = status;

    // Set content type
    if (has_content_type && content_type_len > 0) {
        res->content_type = std::string(content_type, content_type_len);
    } else {
        // Try to determine content type from file extension
        const char *ext = strrchr(file_path, '.');
        if (ext) {
            if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
                res->content_type = "text/html; charset=utf-8";
            } else if (strcmp(ext, ".css") == 0) {
                res->content_type = "text/css; charset=utf-8";
            } else if (strcmp(ext, ".js") == 0) {
                res->content_type = "application/javascript; charset=utf-8";
            } else if (strcmp(ext, ".json") == 0) {
                res->content_type = "application/json; charset=utf-8";
            } else if (strcmp(ext, ".xml") == 0) {
                res->content_type = "application/xml; charset=utf-8";
            } else if (strcmp(ext, ".png") == 0) {
                res->content_type = "image/png";
            } else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
                res->content_type = "image/jpeg";
            } else if (strcmp(ext, ".gif") == 0) {
                res->content_type = "image/gif";
            } else if (strcmp(ext, ".svg") == 0) {
                res->content_type = "image/svg+xml";
            } else if (strcmp(ext, ".pdf") == 0) {
                res->content_type = "application/pdf";
            } else {
                res->content_type = "application/octet-stream";
            }
        } else {
            res->content_type = "application/octet-stream";
        }
    }

    // Add Content-Length header
    res->headers["content-length"] = std::to_string(file_size);
    RETURN_ZVAL(getThis(), 1, 0);
}

// ─── JWT $req->user() / $req->hasRole() ───────────────────────────────────────
PHP_METHOD(KislayRequest, user) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    if (req->jwt_valid && Z_TYPE(req->jwt_payload) != IS_NULL) {
        RETURN_ZVAL(&req->jwt_payload, 1, 0);
    }
    RETURN_NULL();
}

PHP_METHOD(KislayRequest, hasRole) {
    char *role = nullptr; size_t role_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(role, role_len)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    if (!req->jwt_valid || Z_TYPE(req->jwt_payload) != IS_ARRAY) {
        RETURN_FALSE;
    }
    zval *roles_zv = zend_hash_str_find(Z_ARRVAL(req->jwt_payload), "roles", 5);
    if (!roles_zv) roles_zv = zend_hash_str_find(Z_ARRVAL(req->jwt_payload), "role", 4);
    if (!roles_zv) { RETURN_FALSE; }
    if (Z_TYPE_P(roles_zv) == IS_STRING) {
        RETURN_BOOL(strncmp(Z_STRVAL_P(roles_zv), role, role_len) == 0);
    }
    if (Z_TYPE_P(roles_zv) == IS_ARRAY) {
        zval *item; ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(roles_zv), item) {
            if (Z_TYPE_P(item) == IS_STRING && strncmp(Z_STRVAL_P(item), role, role_len) == 0) {
                RETURN_TRUE;
            }
        } ZEND_HASH_FOREACH_END();
    }
    RETURN_FALSE;
}

// ─── W3C Trace Context methods on KislayRequest ──────────────────────────────

PHP_METHOD(KislayRequest, traceId) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->trace_id.c_str());
}

PHP_METHOD(KislayRequest, spanId) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->span_id.c_str());
}

PHP_METHOD(KislayRequest, traceparent) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->traceparent.c_str());
}

PHP_METHOD(KislayRequest, tracestate) {
    ZEND_PARSE_PARAMETERS_NONE();
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(req->tracestate.c_str());
}

// ─── Task Scheduler methods ────────────────────────────────────────────────────
PHP_METHOD(KislayApp, every) {
    zend_long interval_ms; zval *cb;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(interval_ms)
        Z_PARAM_ZVAL(cb)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->async_enabled = true;
    if (app->php_runtime_pool &&
        app->php_runtime_pool->runtime_threads() > 1) {
        zend_throw_exception(zend_ce_exception,
                             "Scheduler callbacks are not available when KISLAY_ENABLE_ZTS_PARALLEL=1",
                             0);
        return;
    }
    const auto task_id = kislay_next_async_id(app);
    kislay_scheduled_task task;
    task.task_id = task_id;
    task.type = kislay_scheduled_task::INTERVAL;
    task.interval_ms = (long)interval_ms;
    task.next_run_ms = kislay_now_ms() + interval_ms;
    task.fired = false;
    zval callback_copy;
    ZVAL_COPY(&callback_copy, cb);
    app->scheduled_callbacks.emplace(task_id, callback_copy);
    {
        std::lock_guard<std::mutex> lock(*app->scheduler_lock);
        app->scheduled_tasks.push_back(std::move(task));
        if (!app->scheduler_running.exchange(true, std::memory_order_acq_rel)) {
            app->scheduler_thread.reset(new std::thread([app]() { kislay_run_scheduler(app); }));
        }
    }
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayApp, once) {
    zend_long delay_ms; zval *cb;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(delay_ms)
        Z_PARAM_ZVAL(cb)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->async_enabled = true;
    if (app->php_runtime_pool &&
        app->php_runtime_pool->runtime_threads() > 1) {
        zend_throw_exception(zend_ce_exception,
                             "Scheduler callbacks are not available when KISLAY_ENABLE_ZTS_PARALLEL=1",
                             0);
        return;
    }
    const auto task_id = kislay_next_async_id(app);
    kislay_scheduled_task task;
    task.task_id = task_id;
    task.type = kislay_scheduled_task::ONCE;
    task.interval_ms = 0;
    task.next_run_ms = kislay_now_ms() + delay_ms;
    task.fired = false;
    zval callback_copy;
    ZVAL_COPY(&callback_copy, cb);
    app->scheduled_callbacks.emplace(task_id, callback_copy);
    {
        std::lock_guard<std::mutex> lock(*app->scheduler_lock);
        app->scheduled_tasks.push_back(std::move(task));
        if (!app->scheduler_running.exchange(true, std::memory_order_acq_rel)) {
            app->scheduler_thread.reset(new std::thread([app]() { kislay_run_scheduler(app); }));
        }
    }
    RETURN_ZVAL(getThis(), 1, 0);
}

// Forward declarations — cron helpers defined later in file
static bool kislay_cron_field_matches(const std::string &field, int value, int min_val, int max_val);
static long long kislay_cron_next_ms(const std::string &expr, long long from_ms);

PHP_METHOD(KislayApp, schedule) {
    char *cron_expr = nullptr; size_t cron_len = 0; zval *cb;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(cron_expr, cron_len)
        Z_PARAM_ZVAL(cb)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->async_enabled = true;
    if (app->php_runtime_pool &&
        app->php_runtime_pool->runtime_threads() > 1) {
        zend_throw_exception(zend_ce_exception,
                             "Scheduler callbacks are not available when KISLAY_ENABLE_ZTS_PARALLEL=1",
                             0);
        return;
    }
    const auto task_id = kislay_next_async_id(app);
    kislay_scheduled_task task;
    task.task_id = task_id;
    task.type = kislay_scheduled_task::CRON;
    task.cron = std::string(cron_expr, cron_len);
    task.interval_ms = 60000; // kept for compatibility
    task.next_run_ms = kislay_cron_next_ms(task.cron, kislay_now_ms());
    task.fired = false;
    zval callback_copy;
    ZVAL_COPY(&callback_copy, cb);
    app->scheduled_callbacks.emplace(task_id, callback_copy);
    {
        std::lock_guard<std::mutex> lock(*app->scheduler_lock);
        app->scheduled_tasks.push_back(std::move(task));
        if (!app->scheduler_running.exchange(true, std::memory_order_acq_rel)) {
            app->scheduler_thread.reset(new std::thread([app]() { kislay_run_scheduler(app); }));
        }
    }
    RETURN_ZVAL(getThis(), 1, 0);
}

// ─── Sub-app mount ─────────────────────────────────────────────────────────────
PHP_METHOD(KislayApp, mount) {
    char *prefix = nullptr; size_t prefix_len = 0; zval *sub_app;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(prefix, prefix_len)
        Z_PARAM_ZVAL(sub_app)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    std::string pfx(prefix, prefix_len);
    if (pfx.empty() || pfx[0] != '/') pfx = "/" + pfx;
    // Remove trailing slash for prefix matching
    while (pfx.size() > 1 && pfx.back() == '/') pfx.pop_back();
    zval copy; ZVAL_COPY(&copy, sub_app);
    app->lock->lock();
    app->mounts.push_back({pfx, copy});
    app->lock->unlock();
    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayApp, __construct) {
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(KislayApp, setOption) {
    char *name = nullptr;
    size_t name_len = 0;
    zval *value = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        php_error_docref(nullptr, E_WARNING, "Cannot set options while server is running; ignoring update");
        RETURN_FALSE;
    }

    std::string key = kislay_to_lower(std::string(name, name_len));
    if (key == "num_threads" || key == "threads") {
        zend_long requested = zval_get_long(value);
        app->thread_count = kislay_sanitize_thread_count(
            requested,
            KISLAYPHP_EXTENSION_G(http_threads),
            "Kislay\\Core\\App::setOption(num_threads)"
        );
        RETURN_TRUE;
    }
    if (key == "workers" || key == "worker_count") {
        app->worker_count = static_cast<int>(zval_get_long(value));
        if (app->worker_count < 1) app->worker_count = 1;
        RETURN_TRUE;
    }
    if (key == "request_timeout_ms" || key == "read_timeout_ms") {
        app->read_timeout_ms = kislay_sanitize_timeout_ms(
            zval_get_long(value),
            KISLAYPHP_EXTENSION_G(read_timeout_ms),
            "Kislay\\Core\\App::setOption(request_timeout_ms)"
        );
        RETURN_TRUE;
    }
    if (key == "max_request_size" || key == "max_body" || key == "max_body_bytes") {
        app->max_body_bytes = kislay_sanitize_max_body(
            zval_get_long(value),
            KISLAYPHP_EXTENSION_G(max_body),
            "Kislay\\Core\\App::setOption(max_body)"
        );
        RETURN_TRUE;
    }
    if (key == "cors" || key == "cors_enabled") {
        app->cors_enabled = kislay_zval_to_bool(value, app->cors_enabled, "Kislay\\Core\\App::setOption(cors)");
        RETURN_TRUE;
    }
    if (key == "keep_alive") {
        app->enable_keep_alive = kislay_zval_to_bool(value, app->enable_keep_alive, "Kislay\\Core\\App::setOption(keep_alive)");
        RETURN_TRUE;
    }
    if (key == "keep_alive_timeout_ms") {
        app->keep_alive_timeout_ms = zval_get_long(value);
        RETURN_TRUE;
    }
    if (key == "referrer_policy" || key == "referrer-policy") {
        zend_string *s = zval_get_string(value);
        std::string candidate(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        app->referrer_policy = kislay_sanitize_referrer_policy(
            candidate,
            app->referrer_policy,
            "Kislay\\Core\\App::setOption(referrer_policy)"
        );
        RETURN_TRUE;
    }
    if (key == "log" || key == "log_enabled") {
        app->log_enabled = kislay_zval_to_bool(value, app->log_enabled, "Kislay\\Core\\App::setOption(log)");
        RETURN_TRUE;
    }
    if (key == "request_id" || key == "request_id_enabled") {
        app->request_id_enabled = kislay_zval_to_bool(value, app->request_id_enabled, "Kislay\\Core\\App::setOption(request_id)");
        RETURN_TRUE;
    }
    if (key == "trace" || key == "trace_enabled" || key == "tracing") {
        app->trace_enabled = kislay_zval_to_bool(value, app->trace_enabled, "Kislay\\Core\\App::setOption(trace)");
        RETURN_TRUE;
    }
    if (key == "async" || key == "async_enabled") {
        app->async_enabled = kislay_zval_to_bool(value, app->async_enabled, "Kislay\\Core\\App::setOption(async)");
        RETURN_TRUE;
    }
    if (key == "gc_every" || key == "gc_interval" || key == "gc_interval_requests") {
        zend_long interval = zval_get_long(value);
        if (interval < 1) {
            interval = 1000;
        }
        app->gc_interval_requests = static_cast<std::uint32_t>(interval);
        RETURN_TRUE;
    }
    if (key == "async_threads" || key == "async_worker_threads") {
        app->async_worker_count = static_cast<int>(zval_get_long(value));
        if (app->async_worker_count < 1) app->async_worker_count = 1;
        RETURN_TRUE;
    }
    if (key == "tls_cert") {
        zend_string *s = zval_get_string(value);
        std::string candidate(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        if (!candidate.empty() && (!kislay_path_is_regular_file(candidate) || access(candidate.c_str(), R_OK) != 0)) {
            php_error_docref(nullptr, E_WARNING,
                             "Kislay\\Core\\App::setOption(tls_cert): \"%s\" is not readable; keeping previous value",
                             candidate.c_str());
            RETURN_TRUE;
        }
        app->default_tls_cert = candidate;
        RETURN_TRUE;
    }
    if (key == "tls_key") {
        zend_string *s = zval_get_string(value);
        std::string candidate(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        if (!candidate.empty() && (!kislay_path_is_regular_file(candidate) || access(candidate.c_str(), R_OK) != 0)) {
            php_error_docref(nullptr, E_WARNING,
                             "Kislay\\Core\\App::setOption(tls_key): \"%s\" is not readable; keeping previous value",
                             candidate.c_str());
            RETURN_TRUE;
        }
        app->default_tls_key = candidate;
        RETURN_TRUE;
    }
    if (key == "max_requests") {
        app->max_requests = static_cast<std::uint32_t>(zval_get_long(value));
        RETURN_TRUE;
    }
    if (key == "queue_size" || key == "request_queue_size") {
        app->queue_size = static_cast<std::size_t>(zval_get_long(value));
        RETURN_TRUE;
    }
    if (key == "server_type" || key == "runtime_server") {
        zend_string *s = zval_get_string(value);
        std::string type = kislay_to_lower(std::string(ZSTR_VAL(s), ZSTR_LEN(s)));
        zend_string_release(s);
        if (type == "libuv" || type == "uv" || type == "eventloop") {
            app->server_type = "libuv";
        } else {
            app->server_type = "civetweb";
        }
        RETURN_TRUE;
    }
    if (key == "document_root") {
        zend_string *s = zval_get_string(value);
        std::string candidate(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        app->document_root = kislay_sanitize_document_root(
            candidate,
            app->document_root,
            "Kislay\\Core\\App::setOption(document_root)"
        );
        RETURN_TRUE;
    }

    if (key == "jwt_secret") {
        zend_string *s = zval_get_string(value);
        app->jwt_secret = std::string(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        if (!app->jwt_secret.empty()) app->jwt_enabled = true;
        RETURN_TRUE;
    }
    if (key == "jwt_required") {
        app->jwt_required = kislay_zval_to_bool(value, app->jwt_required, "Kislay\\Core\\App::setOption(jwt_required)");
        RETURN_TRUE;
    }
    if (key == "jwt_exclude") {
        app->jwt_exclude_prefixes.clear();
        if (Z_TYPE_P(value) == IS_ARRAY) {
            zval *item; ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), item) {
                if (Z_TYPE_P(item) == IS_STRING) {
                    app->jwt_exclude_prefixes.push_back(std::string(Z_STRVAL_P(item), Z_STRLEN_P(item)));
                }
            } ZEND_HASH_FOREACH_END();
        } else if (Z_TYPE_P(value) == IS_STRING) {
            std::string csv(Z_STRVAL_P(value), Z_STRLEN_P(value));
            std::istringstream ss(csv);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                tok.erase(0, tok.find_first_not_of(" "));
                if (tok.find_last_not_of(" ") != std::string::npos)
                    tok.erase(tok.find_last_not_of(" ") + 1);
                if (!tok.empty()) app->jwt_exclude_prefixes.push_back(tok);
            }
        }
        RETURN_TRUE;
    }
    if (key == "actuator" || key == "actuator_enabled") {
        app->actuator_enabled = kislay_zval_to_bool(value, app->actuator_enabled, "Kislay\\Core\\App::setOption(actuator)");
        RETURN_TRUE;
    }

    php_error_docref(nullptr, E_WARNING, "Unsupported option \"%s\"; ignoring and keeping defaults", name);
    RETURN_TRUE;
}

static bool kislay_app_add_route(php_kislay_app_t *app, const std::string &method, const std::string &pattern, zval *handler) {
    if (!kislay_is_callable(handler)) {
        zend_throw_exception(zend_ce_exception, "Route handler must be callable", 0);
        return false;
    }

    kislay::Route route;
    route.method = kislay_to_upper(method);
    if (route.method == "ALL") {
        route.method = "*";
    }
    std::string full_pattern = pattern;
    if (!app->group_stack.empty()) {
        std::string prefix;
        for (const auto &ctx : app->group_stack) {
            prefix = kislay_join_paths(prefix, ctx.prefix);
        }
        full_pattern = kislay_join_paths(prefix, pattern);
    }
    route.pattern = kislay_normalize_route_path(full_pattern);
    std::string route_error;
    if (!kislay_build_route(route.pattern, &route, &route_error)) {
        zend_throw_exception(zend_ce_exception,
                             route_error.empty() ? "Invalid route pattern" : route_error.c_str(),
                             0);
        return false;
    }

    for (const auto &ctx : app->group_stack) {
        for (const auto &mw : ctx.middleware) {
            zval copy;
            ZVAL_COPY(&copy, const_cast<zval *>(&mw));
            route.middleware.push_back(copy);
        }
    }

    ZVAL_COPY(&route.handler, handler);
    size_t index = app->routes.size();
    app->routes.push_back(std::move(route));
    kislay_route_compile_middleware(app, &app->routes[index]);
    if (app->routes[index].exact) {
        app->exact_routes_by_method[app->routes[index].method][app->routes[index].pattern] = index;
        if (app->log_enabled) {
            std::fprintf(stderr, "[kislay-debug] Registered Exact: Method=%s, Pattern=%s, Index=%zu\n",
                         app->routes[index].method.c_str(), app->routes[index].pattern.c_str(), index);
        }
    } else {        app->segmented_routes_by_method[app->routes[index].method][app->routes[index].segments.size()].push_back(index);
        if (app->log_enabled) {
            std::fprintf(stderr, "[kislay-debug] Registered Segmented: Method=%s, Pattern=%s, Index=%zu, Segs=%zu\n", 
                         app->routes[index].method.c_str(), app->routes[index].pattern.c_str(), index, app->routes[index].segments.size());
        }
    }
    return true;
}

static bool kislay_app_add_hook(php_kislay_app_t *app, std::vector<zval> &hooks, zval *hook, const char *context) {
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register request hooks while server is running", 0);
        return false;
    }
    if (!kislay_is_callable(hook)) {
        zend_throw_exception(zend_ce_exception, "Hook must be callable", 0);
        return false;
    }

    std::lock_guard<std::mutex> guard(*app->lock);
    zval copy;
    ZVAL_COPY(&copy, hook);
    hooks.push_back(copy);
    (void) context;
    return true;
}

PHP_METHOD(KislayApp, use) {
    zval *path_or_callable = nullptr;
    zval *callable = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(path_or_callable)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(callable)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register middleware while server is running", 0);
        RETURN_FALSE;
    }

    if (ZEND_NUM_ARGS() == 1) {
        if (!kislay_is_callable(path_or_callable)) {
            zend_throw_exception(zend_ce_exception, "Middleware must be callable", 0);
            RETURN_FALSE;
        }
        // Detect arity to determine if this is an error handler (4 params)
        bool is_error_handler = false;
        zval *cb = path_or_callable;
        if (Z_TYPE_P(cb) == IS_OBJECT) {
            zend_function *efn = static_cast<zend_function *>(zend_hash_str_find_ptr(
                &Z_OBJCE_P(cb)->function_table, "__invoke", sizeof("__invoke") - 1));
            if (efn) is_error_handler = (efn->common.num_args >= 4);
        } else if (Z_TYPE_P(cb) == IS_STRING) {
            zend_function *efn = (zend_function *)zend_hash_find_ptr(
                EG(function_table), Z_STR_P(cb));
            if (efn) is_error_handler = (efn->common.num_args >= 4);
        }
        std::lock_guard<std::mutex> guard(*app->lock);
        zval copy;
        ZVAL_COPY(&copy, path_or_callable);
        if (is_error_handler) {
            app->error_handlers.push_back(copy);
        } else {
            app->middleware.push_back(copy);
            kislay_app_rebuild_compiled_middleware(app);
        }
        RETURN_TRUE;
    }

    if (Z_TYPE_P(path_or_callable) != IS_STRING || !kislay_is_callable(callable)) {
        zend_throw_exception(zend_ce_exception, "use(path, middleware) expects a string path and callable middleware", 0);
        RETURN_FALSE;
    }

    std::string prefix = kislay_normalize_prefix(std::string(Z_STRVAL_P(path_or_callable), Z_STRLEN_P(path_or_callable)));
    if (prefix.empty()) {
        prefix = "/";
    }

    std::lock_guard<std::mutex> guard(*app->lock);
    kislay::PathMiddleware scoped;
    scoped.prefix = prefix;
    ZVAL_COPY(&scoped.middleware, callable);
    app->path_middleware.push_back(std::move(scoped));
    kislay_app_rebuild_compiled_middleware(app);
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, get) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "GET", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, post) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "POST", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, put) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "PUT", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, patch) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "PATCH", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, delete) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "DELETE", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, options) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "OPTIONS", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, all) {
    char *path = nullptr;
    size_t path_len = 0;
    zval *handler = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        zend_throw_exception(zend_ce_exception, "Cannot register routes while server is running", 0);
        RETURN_FALSE;
    }
    std::lock_guard<std::mutex> guard(*app->lock);
    if (!kislay_app_add_route(app, "ALL", std::string(path, path_len), handler)) {
        if (!EG(exception)) {
            zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
        }
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, onRequestStart) {
    zval *hook = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(hook)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_app_add_hook(app, app->request_start_hooks, hook, "onRequestStart")) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, onRequestEnd) {
    zval *hook = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(hook)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (!kislay_app_add_hook(app, app->request_end_hooks, hook, "onRequestEnd")) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, group) {
    char *prefix = nullptr;
    size_t prefix_len = 0;
    zval *callback = nullptr;
    zval *middleware = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(prefix, prefix_len)
        Z_PARAM_ZVAL(callback)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(middleware)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(callback)) {
        zend_throw_exception(zend_ce_exception, "Group callback must be callable", 0);
        RETURN_FALSE;
    }

    kislay::GroupContext ctx;
    ctx.prefix = kislay_normalize_prefix(std::string(prefix, prefix_len));

    if (middleware != nullptr) {
        HashTable *ht = Z_ARRVAL_P(middleware);
        zval *entry = nullptr;
        ZEND_HASH_FOREACH_VAL(ht, entry) {
            if (!kislay_is_callable(entry)) {
                zend_throw_exception(zend_ce_exception, "Group middleware must be callable", 0);
                for (auto &mw : ctx.middleware) {
                    zval_ptr_dtor(&mw);
                }
                RETURN_FALSE;
            }
            zval copy;
            ZVAL_COPY(&copy, entry);
            ctx.middleware.push_back(copy);
        } ZEND_HASH_FOREACH_END();
    }

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (kislay_app_is_running(app)) {
        for (auto &mw : ctx.middleware) {
            zval_ptr_dtor(&mw);
        }
        zend_throw_exception(zend_ce_exception, "Cannot register groups while server is running", 0);
        RETURN_FALSE;
    }
    {
        std::lock_guard<std::mutex> guard(*app->lock);
        app->group_stack.push_back(std::move(ctx));
    }

    zval args[1];
    ZVAL_COPY(&args[0], getThis());
    zval retval;
    bool ok = kislay_call_php(callback, 1, args, &retval);
    zval_ptr_dtor(&args[0]);
    if (ok) {
        zval_ptr_dtor(&retval);
    }

    {
        std::lock_guard<std::mutex> guard(*app->lock);
        if (!app->group_stack.empty()) {
            kislay::GroupContext last = std::move(app->group_stack.back());
            app->group_stack.pop_back();
            for (auto &mw : last.middleware) {
                zval_ptr_dtor(&mw);
            }
        }
    }

    if (!ok || EG(exception)) {
        if (EG(exception)) {
            zend_clear_exception();
        }
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayApp, listen) {
    char *host = nullptr;
    size_t host_len = 0;
    zend_long port = 0;
    zval *tls = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(tls)
    ZEND_PARSE_PARAMETERS_END();

    if (port <= 0 || port > 65535) {
        zend_throw_exception(zend_ce_exception, "Invalid port", 0);
        RETURN_FALSE;
    }

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (app->ctx != nullptr) {
        zend_throw_exception(zend_ce_exception, "Server already running", 0);
        RETURN_FALSE;
    }

    std::string listen_addr;
    if (host_len > 0) {
        listen_addr = std::string(host, host_len) + ":" + std::to_string(port);
    } else {
        listen_addr = std::to_string(port);
    }

    std::string cert_path;
    std::string key_path;
    if (tls != nullptr) {
        zval *cert = zend_hash_str_find(Z_ARRVAL_P(tls), "cert", sizeof("cert") - 1);
        zval *key = zend_hash_str_find(Z_ARRVAL_P(tls), "key", sizeof("key") - 1);
        if (cert && Z_TYPE_P(cert) == IS_STRING) {
            cert_path = std::string(Z_STRVAL_P(cert), Z_STRLEN_P(cert));
        }
        if (key && Z_TYPE_P(key) == IS_STRING) {
            key_path = std::string(Z_STRVAL_P(key), Z_STRLEN_P(key));
        }
    }
    if (cert_path.empty()) {
        cert_path = app->default_tls_cert;
    }
    if (key_path.empty()) {
        key_path = app->default_tls_key;
    }
    app->document_root = kislay_sanitize_document_root(app->document_root, "", "Kislay\\Core\\App::listen");
    kislay_disable_stack_guard_for_nts("Kislay\\Core\\App::listen");
    kislay_sanitize_tls_paths(&cert_path, &key_path, "Kislay\\Core\\App::listen");
    app->thread_count = kislay_sanitize_thread_count(app->thread_count, KISLAYPHP_EXTENSION_G(http_threads), "Kislay\\Core\\App::listen");
    app->read_timeout_ms = kislay_sanitize_timeout_ms(app->read_timeout_ms, KISLAYPHP_EXTENSION_G(read_timeout_ms), "Kislay\\Core\\App::listen");

    bool is_worker = false;
    std::vector<pid_t> children;
    if (app->worker_count > 1) {
        for (int i = 0; i < app->worker_count - 1; ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                is_worker = true;
                break;
            } else if (pid > 0) {
                children.push_back(pid);
            } else {
                php_error_docref(nullptr, E_WARNING, "fork() failed");
            }
        }
    }

    if (!kislay_app_start_runtime(app, false)) {
        zend_throw_exception(zend_ce_exception, "Failed to start PHP runtime", 0);
        RETURN_FALSE;
    }

    if (app->server_type == "libuv") {
        if (!kislay_app_start_server_uv(app, listen_addr)) {
            kislay_app_stop_runtime(app);
            zend_throw_exception(zend_ce_exception, "Failed to start libuv server", 0);
            RETURN_FALSE;
        }
    } else {
        if (!kislay_app_start_server(app, listen_addr, cert_path, key_path)) {
            kislay_app_stop_runtime(app);
            zend_throw_exception(zend_ce_exception, "Failed to start server", 0);
            RETURN_FALSE;
        }
    }

    // AsyncBridge is started in start_runtime
    kislay_app_wait_loop(app, -1);

    if (app->worker_count > 1) {
        if (!is_worker) {
            // kislay_app_wait_loop() above returns once *this* (master) process's
            // running flag flips - whether from SIGTERM/SIGINT delivered to the
            // master's own PID, or an explicit $app->stop() call. Either way the
            // forked children are separate PIDs that never received anything, so
            // without this they'd sit in their own wait loops forever and the
            // waitpid() calls below would block indefinitely (confirmed via a
            // manual repro: the master ends up parked in __wait4 at this exact
            // line while children remain alive and unsignaled).
            for (pid_t child_pid : children) {
                if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
                    php_error_docref(nullptr, E_WARNING, "Failed to signal worker pid %d: %s", child_pid, strerror(errno));
                }
            }
            for (pid_t child_pid : children) {
                int status;
                waitpid(child_pid, &status, 0);
            }
        } else {
            exit(0);
        }
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayApp, listenAsync) {
    char *host = nullptr;
    size_t host_len = 0;
    zend_long port = 0;
    zval *tls = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(tls)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (!app->is_zts_runtime) {
        zend_throw_exception(zend_ce_exception, "listenAsync() requires ZTS. In NTS mode, use listen().", 0);
        RETURN_FALSE;
    }

    if (port <= 0 || port > 65535) {
        zend_throw_exception(zend_ce_exception, "Invalid port", 0);
        RETURN_FALSE;
    }

    if (app->ctx != nullptr) {
        zend_throw_exception(zend_ce_exception, "Server already running", 0);
        RETURN_FALSE;
    }

    std::string listen_addr;
    if (host_len > 0) {
        listen_addr = std::string(host, host_len) + ":" + std::to_string(port);
    } else {
        listen_addr = std::to_string(port);
    }

    std::string cert_path;
    std::string key_path;
    if (tls != nullptr) {
        zval *cert = zend_hash_str_find(Z_ARRVAL_P(tls), "cert", sizeof("cert") - 1);
        zval *key = zend_hash_str_find(Z_ARRVAL_P(tls), "key", sizeof("key") - 1);
        if (cert && Z_TYPE_P(cert) == IS_STRING) {
            cert_path = std::string(Z_STRVAL_P(cert), Z_STRLEN_P(cert));
        }
        if (key && Z_TYPE_P(key) == IS_STRING) {
            key_path = std::string(Z_STRVAL_P(key), Z_STRLEN_P(key));
        }
    }
    if (cert_path.empty()) {
        cert_path = app->default_tls_cert;
    }
    if (key_path.empty()) {
        key_path = app->default_tls_key;
    }
    app->document_root = kislay_sanitize_document_root(app->document_root, "", "Kislay\\Core\\App::listenAsync");
    kislay_disable_stack_guard_for_nts("Kislay\\Core\\App::listenAsync");
    kislay_sanitize_tls_paths(&cert_path, &key_path, "Kislay\\Core\\App::listenAsync");
    app->thread_count = kislay_sanitize_thread_count(app->thread_count, KISLAYPHP_EXTENSION_G(http_threads), "Kislay\\Core\\App::listenAsync");
    app->read_timeout_ms = kislay_sanitize_timeout_ms(app->read_timeout_ms, KISLAYPHP_EXTENSION_G(read_timeout_ms), "Kislay\\Core\\App::listenAsync");

    if (!kislay_app_start_runtime(app, false)) {
        zend_throw_exception(zend_ce_exception, "Failed to start PHP runtime", 0);
        RETURN_FALSE;
    }

    if (app->server_type == "libuv") {
        if (!kislay_app_start_server_uv(app, listen_addr)) {
            kislay_app_stop_runtime(app);
            zend_throw_exception(zend_ce_exception, "Failed to start libuv server", 0);
            RETURN_FALSE;
        }
    } else {
        if (!kislay_app_start_server(app, listen_addr, cert_path, key_path)) {
            kislay_app_stop_runtime(app);
            zend_throw_exception(zend_ce_exception, "Failed to start server", 0);
            RETURN_FALSE;
        }
    }

    // AsyncBridge is started in start_runtime
    if (app->server_type == "libuv") { // Actually this is for listenAsync, let's just make it return true if not blocking
        RETURN_TRUE;
    }
    kislay_app_wait_loop(app, -1);
}

PHP_METHOD(KislayApp, wait) {
    zend_long timeout_ms = -1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();

    if (timeout_ms < -1) {
        zend_throw_exception(zend_ce_exception, "timeoutMs must be >= -1", 0);
        RETURN_FALSE;
    }

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (!app->running.load(std::memory_order_relaxed)) {
        RETURN_TRUE;
    }

    if (kislay_app_wait_loop(app, timeout_ms)) {
        RETURN_TRUE;
    }

    RETURN_FALSE;
}

PHP_METHOD(KislayApp, isRunning) {
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    RETURN_BOOL(app->running.load(std::memory_order_relaxed) ? 1 : 0);
}

PHP_METHOD(KislayApp, stop) {
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    kislay_app_stop_server(app);
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, onNotFound) {
    zval *handler;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (app->has_not_found_handler) { zval_ptr_dtor(&app->not_found_handler); }
    ZVAL_COPY(&app->not_found_handler, handler);
    app->has_not_found_handler = true;
}

PHP_METHOD(KislayApp, onError) {
    zval *handler;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (app->has_unhandled_error_handler) { zval_ptr_dtor(&app->unhandled_error_handler); }
    ZVAL_COPY(&app->unhandled_error_handler, handler);
    app->has_unhandled_error_handler = true;
}

PHP_METHOD(KislayApp, addHealthIndicator) {
    zval *indicator;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(indicator)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    zval copy;
    ZVAL_COPY(&copy, indicator);
    app->health_indicators.push_back(copy);
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, setMemoryLimit) {
    zend_long bytes = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    if (bytes < 0) {
        php_error_docref(nullptr, E_WARNING,
                         "Kislay\\Core\\App::setMemoryLimit: invalid value %lld; using 0 (disabled)",
                         static_cast<long long>(bytes));
        bytes = 0;
    }

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->memory_limit_bytes = static_cast<size_t>(bytes);
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, getMemoryLimit) {
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    RETURN_LONG(static_cast<zend_long>(app->memory_limit_bytes));
}

PHP_METHOD(KislayApp, enableGc) {
    zend_bool enabled = 1;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->gc_after_request = (enabled != 0);
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, enableAsync) {
    zend_bool enabled = 1;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    app->async_enabled = (enabled != 0);
    RETURN_TRUE;
}

PHP_METHOD(KislayAsyncHttp, __construct) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->curl = curl_easy_init();
    if (!async_http->curl) {
        zend_throw_exception(zend_ce_exception, "Failed to initialize curl", 0);
        RETURN_FALSE;
    }
    async_http->timeout_ms = KISLAYPHP_EXTENSION_G(read_timeout_ms) > 0 ? KISLAYPHP_EXTENSION_G(read_timeout_ms) : 10000;
    curl_easy_setopt(async_http->curl, CURLOPT_TIMEOUT_MS, async_http->timeout_ms);
}

PHP_METHOD(KislayAsyncHttp, get) {
    zend_string *url;
    zval *data = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(data)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->method = "GET";
    async_http->request_body.clear();
    async_http->use_request_body = false;

    std::string target_url(ZSTR_VAL(url), ZSTR_LEN(url));
    if (data != nullptr) {
        std::string query = kislay_async_http_build_query(async_http->curl, Z_ARRVAL_P(data));
        target_url = kislay_async_http_append_query(url, query);
    }

    async_http->url = target_url;
    curl_easy_setopt(async_http->curl, CURLOPT_URL, target_url.c_str());
    curl_easy_setopt(async_http->curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(async_http->curl, CURLOPT_POST, 0L);
    curl_easy_setopt(async_http->curl, CURLOPT_HTTPGET, 1L);
}

PHP_METHOD(KislayAsyncHttp, post) {
    zend_string *url;
    zval *data = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(data)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->method = "POST";
    async_http->url.assign(ZSTR_VAL(url), ZSTR_LEN(url));
    
    curl_easy_setopt(async_http->curl, CURLOPT_URL, ZSTR_VAL(url));
    curl_easy_setopt(async_http->curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(async_http->curl, CURLOPT_POST, 1L);

    async_http->request_body.clear();
    async_http->use_request_body = true;
    if (data) {
        smart_str body = {0};
        php_json_encode(&body, data, 0);
        smart_str_0(&body);
        if (body.s != nullptr) {
            async_http->request_body.assign(ZSTR_VAL(body.s), ZSTR_LEN(body.s));
        }
        smart_str_free(&body);
    }
}

PHP_METHOD(KislayAsyncHttp, put) {
    zend_string *url;
    zval *data = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(data)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->method = "PUT";
    async_http->url.assign(ZSTR_VAL(url), ZSTR_LEN(url));
    
    curl_easy_setopt(async_http->curl, CURLOPT_URL, ZSTR_VAL(url));
    curl_easy_setopt(async_http->curl, CURLOPT_POST, 0L);
    curl_easy_setopt(async_http->curl, CURLOPT_CUSTOMREQUEST, "PUT");

    async_http->request_body.clear();
    async_http->use_request_body = true;
    if (data) {
        smart_str body = {0};
        php_json_encode(&body, data, 0);
        smart_str_0(&body);
        if (body.s != nullptr) {
            async_http->request_body.assign(ZSTR_VAL(body.s), ZSTR_LEN(body.s));
        }
        smart_str_free(&body);
    }
}

PHP_METHOD(KislayAsyncHttp, patch) {
    zend_string *url;
    zval *data = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(data)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->method = "PATCH";
    async_http->url.assign(ZSTR_VAL(url), ZSTR_LEN(url));
    
    curl_easy_setopt(async_http->curl, CURLOPT_URL, ZSTR_VAL(url));
    curl_easy_setopt(async_http->curl, CURLOPT_POST, 0L);
    curl_easy_setopt(async_http->curl, CURLOPT_CUSTOMREQUEST, "PATCH");

    async_http->request_body.clear();
    async_http->use_request_body = true;
    if (data) {
        smart_str body = {0};
        php_json_encode(&body, data, 0);
        smart_str_0(&body);
        if (body.s != nullptr) {
            async_http->request_body.assign(ZSTR_VAL(body.s), ZSTR_LEN(body.s));
        }
        smart_str_free(&body);
    }
}

PHP_METHOD(KislayAsyncHttp, delete) {
    zend_string *url;
    zval *data = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(data)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->method = "DELETE";
    async_http->url.assign(ZSTR_VAL(url), ZSTR_LEN(url));
    
    curl_easy_setopt(async_http->curl, CURLOPT_URL, ZSTR_VAL(url));
    curl_easy_setopt(async_http->curl, CURLOPT_POST, 0L);
    curl_easy_setopt(async_http->curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    async_http->request_body.clear();
    async_http->use_request_body = true;
    if (data) {
        smart_str body = {0};
        php_json_encode(&body, data, 0);
        smart_str_0(&body);
        if (body.s != nullptr) {
            async_http->request_body.assign(ZSTR_VAL(body.s), ZSTR_LEN(body.s));
        }
        smart_str_free(&body);
    }
}

PHP_METHOD(KislayAsyncHttp, setHeader) {
    zend_string *name, *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    
    std::string header = std::string(ZSTR_VAL(name)) + ": " + ZSTR_VAL(value);
    async_http->headers = curl_slist_append(async_http->headers, header.c_str());
    async_http->header_lines.push_back(header);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    php_kislay_async_http_t *async_http = (php_kislay_async_http_t *)userp;
    async_http->response_body.append((char*)contents, realsize);
    return realsize;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_async_http_execute, 0, 0, 0)
ZEND_END_ARG_INFO()

static bool kislay_async_http_is_self_request(const php_kislay_async_http_t *async_http,
                                              const php_kislay_request_t *active_req);

PHP_METHOD(KislayAsyncHttp, execute) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    php_kislay_app_t *app = kislay_active_app.load(std::memory_order_relaxed);

    if (app != nullptr &&
        app->php_runtime_pool &&
        app->php_runtime_pool->runtime_threads() == 1 &&
        kislay_async_http_is_self_request(async_http, kislay_active_request)) {
        zend_throw_exception(zend_ce_exception,
                             "Async HTTP self-request is not allowed in single PHP runtime mode",
                             0);
        RETURN_FALSE;
    }
    
    async_http->response_body.clear();
    curl_easy_setopt(async_http->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(async_http->curl, CURLOPT_WRITEDATA, async_http);
    curl_easy_setopt(async_http->curl, CURLOPT_HTTPHEADER, async_http->headers);
    if (async_http->use_request_body) {
        curl_easy_setopt(async_http->curl, CURLOPT_POSTFIELDS, async_http->request_body.c_str());
        curl_easy_setopt(async_http->curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(async_http->request_body.size()));
    }
    
    CURLcode res = curl_easy_perform(async_http->curl);
    if (res != CURLE_OK) {
        zend_throw_exception(zend_ce_exception, curl_easy_strerror(res), 0);
        RETURN_FALSE;
    }
    
    curl_easy_getinfo(async_http->curl, CURLINFO_RESPONSE_CODE, &async_http->response_code);
    RETURN_TRUE;
}

static void kislay_async_http_apply_headers(php_kislay_async_http_t *async_http) {
    php_kislay_request_t *active_req = kislay_active_request;
    if (active_req) {
        auto it = active_req->headers.find("x-correlation-id");
        if (it != active_req->headers.end()) {
            bool found = false;
            struct curl_slist *curr = async_http->headers;
            while (curr) {
                if (strncasecmp(curr->data, "X-Correlation-ID:", 17) == 0) {
                    found = true;
                    break;
                }
                curr = curr->next;
            }
            if (!found) {
                std::string h = "X-Correlation-ID: " + it->second;
                async_http->headers = curl_slist_append(async_http->headers, h.c_str());
                async_http->header_lines.push_back(h);
            }
        }
    }
    curl_easy_setopt(async_http->curl, CURLOPT_HTTPHEADER, async_http->headers);
}

static bool kislay_async_http_is_self_request(const php_kislay_async_http_t *async_http,
                                              const php_kislay_request_t *active_req) {
    if (async_http == nullptr || active_req == nullptr || async_http->url.empty()) {
        return false;
    }
    auto host_it = active_req->headers.find("host");
    if (host_it == active_req->headers.end() || host_it->second.empty()) {
        return false;
    }

    CURLU *url = curl_url();
    if (url == nullptr) {
        return false;
    }

    bool is_self = false;
    char *host = nullptr;
    char *port = nullptr;
    char *scheme = nullptr;
    if (curl_url_set(url, CURLUPART_URL, async_http->url.c_str(), 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK) {
        std::string current_authority = kislay_ascii_lower_copy(host_it->second);
        std::string target_host = kislay_ascii_lower_copy(host != nullptr ? host : "");
        std::string target_authority = target_host;

        if (curl_url_get(url, CURLUPART_PORT, &port, 0) == CURLUE_OK && port != nullptr && *port != '\0') {
            target_authority.push_back(':');
            target_authority.append(port);
        } else if (curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK && scheme != nullptr) {
            std::string normalized_scheme = kislay_ascii_lower_copy(scheme);
            if ((normalized_scheme == "http" && (current_authority == target_host || current_authority == target_host + ":80")) ||
                (normalized_scheme == "https" && (current_authority == target_host || current_authority == target_host + ":443"))) {
                is_self = true;
            }
        }

        if (!is_self) {
            is_self = current_authority == target_authority;
        }
    }

    if (host != nullptr) {
        curl_free(host);
    }
    if (port != nullptr) {
        curl_free(port);
    }
    if (scheme != nullptr) {
        curl_free(scheme);
    }
    curl_url_cleanup(url);
    return is_self;
}

PHP_METHOD(KislayAsyncHttp, executeAsync) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    php_kislay_app_t *app = kislay_active_app.load(std::memory_order_relaxed);
    const std::size_t lane_index = kislay_async_lane_index_for_thread(app);
    KislayAsyncLaneState *lane_state = kislay_async_lane_state(app, lane_index);

    if (!app) {
        zend_throw_exception(zend_ce_exception, "No active Kislay App found for async HTTP execution", 0);
        return;
    }
    if (app->php_runtime_pool &&
        app->php_runtime_pool->runtime_threads() == 1 &&
        kislay_async_http_is_self_request(async_http, kislay_active_request)) {
        zend_throw_exception(zend_ce_exception,
                             "Async HTTP self-request is not allowed in single PHP runtime mode",
                             0);
        return;
    }

    async_http->response_body.clear();
    async_http->retry_count = 0;
    kislay_async_http_apply_headers(async_http);

    object_init_ex(return_value, kislay_promise_ce);
    php_kislay_promise_t *promise = php_kislay_promise_from_obj(Z_OBJ_P(return_value));
    promise->owner_app = app;
    promise->async_id = kislay_next_async_id(app);
    promise->owner_lane = lane_index;
    if (kislay_active_request != nullptr) {
        promise->owner_request_id = kislay_active_request->request_id;
    }
    if (lane_state == nullptr || !lane_state->promise_registry) {
        zend_throw_exception(zend_ce_exception, "Async HTTP lane state is not initialized", 0);
        return;
    }
    lane_state->promise_registry->register_promise(promise->async_id, promise);

    kislay::runtime::HttpRequestTask task;
    task.task_id = promise->async_id;
    task.owner_lane = lane_index;
    task.method = async_http->method;
    task.url = async_http->url;
    task.headers = async_http->header_lines;
    task.body = async_http->use_request_body ? async_http->request_body : std::string();
    task.timeout_ms = async_http->timeout_ms;
    task.max_retries = async_http->max_retries;
    task.retry_delay_ms = async_http->retry_delay_ms;

    GC_ADDREF(&async_http->std);

    KislayPendingHttpTask pending{async_http, promise->owner_request_id};
    auto [pending_it, inserted] = lane_state->pending_http_tasks.emplace(promise->async_id, std::move(pending));
    if (inserted) {
        kislay_async_track_request(lane_state, pending_it->second.request_id);
    }

    if (!app->loop_active.load(std::memory_order_relaxed)) {
        app->loop_active.store(true, std::memory_order_relaxed);
    }
    if (app->async_bridge && !app->async_bridge->running()) {
        app->async_bridge->start(
            static_cast<std::size_t>(std::max(app->async_worker_count, 1)),
            std::max(app->php_runtime_threads, std::size_t{1})
        );
    }
    if (!app->async_bridge || !app->async_bridge->submit_http(std::move(task))) {
        auto it = lane_state->pending_http_tasks.find(promise->async_id);
        if (it != lane_state->pending_http_tasks.end()) {
            kislay_async_untrack_request(lane_state, it->second.request_id);
            OBJ_RELEASE(&it->second.async_http->std);
            lane_state->pending_http_tasks.erase(it);
        }
        lane_state->promise_registry->unregister_promise(promise->async_id);
        zend_throw_exception(zend_ce_exception, "Async bridge is not running or overloaded", 0);
        return;
    }
}

PHP_METHOD(KislayAsyncHttp, retry) {
    zend_long max_retries;
    zend_long delay_ms = 1000;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_LONG(max_retries)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(delay_ms)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    async_http->max_retries = static_cast<int>(max_retries);
    async_http->retry_delay_ms = static_cast<int>(delay_ms);
    async_http->retry_count = 0;

    RETURN_OBJ(Z_OBJ_P(getThis()));
    GC_ADDREF(Z_OBJ_P(getThis()));
}

PHP_METHOD(KislayAsyncHttp, getResponse) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(async_http->response_body.c_str());
}

PHP_METHOD(KislayAsyncHttp, getResponseCode) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    RETURN_LONG(async_http->response_code);
}

static zend_object_handlers kislay_promise_handlers;
static void kislay_promise_dispatch(php_kislay_promise_t *promise);
static void kislay_promise_invoke_callback(zval *callback, php_kislay_promise_t *promise);

static zend_object *kislay_promise_create_object(zend_class_entry *ce) {
    php_kislay_promise_t *promise = static_cast<php_kislay_promise_t *>(
        ecalloc(1, sizeof(php_kislay_promise_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&promise->std, ce);
    object_properties_init(&promise->std, ce);
    promise->state = PromiseState::Pending;
    ZVAL_UNDEF(&promise->result);
    promise->owner_app = nullptr;
    new (&promise->owner_request_id) std::string();
    promise->async_id = 0;
    promise->owner_lane = 0;
    promise->std.handlers = &kislay_promise_handlers;
    return &promise->std;
}

static void kislay_promise_free_obj(zend_object *object) {
    php_kislay_promise_t *promise = php_kislay_promise_from_obj(object);
    zval_ptr_dtor(&promise->result);
    promise->owner_request_id.~basic_string();
    zend_object_std_dtor(&promise->std);
}

static zend_object *kislay_service_client_create_object(zend_class_entry *ce) {
    php_kislay_service_client_t *obj = static_cast<php_kislay_service_client_t *>(
        ecalloc(1, sizeof(php_kislay_service_client_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    new (&obj->base_url) std::string();
    new (&obj->service_name) std::string();
    new (&obj->default_headers) std::map<std::string, std::string>();
    obj->timeout_ms = 5000;
    obj->retry_count = 0;
    obj->retry_delay_ms = 100;
    pthread_mutex_init(&obj->lock, nullptr);
    obj->std.handlers = &kislay_service_client_handlers;
    return &obj->std;
}

static void kislay_service_client_free_obj(zend_object *object) {
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(object);
    obj->base_url.~basic_string();
    obj->service_name.~basic_string();
    obj->default_headers.~map();
    pthread_mutex_destroy(&obj->lock);
    zend_object_std_dtor(&obj->std);
}

static void kislay_promise_resolve(php_kislay_promise_t *promise, zval *value) {
    if (promise->state != PromiseState::Pending) return;
    promise->state = PromiseState::Fulfilled;
    ZVAL_COPY(&promise->result, value);
}

static void kislay_promise_reject(php_kislay_promise_t *promise, zval *reason) {
    if (promise->state != PromiseState::Pending) return;
    promise->state = PromiseState::Rejected;
    ZVAL_COPY(&promise->result, reason);
}

PHP_METHOD(KislayPromise, then) {
    zval *on_fulfilled = nullptr;
    zval *on_rejected = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(on_fulfilled)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(on_rejected)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(on_fulfilled)) {
        zend_throw_exception(zend_ce_exception, "onFulfilled must be callable", 0);
        return;
    }
    if (on_rejected && !kislay_is_callable(on_rejected)) {
        zend_throw_exception(zend_ce_exception, "onRejected must be callable", 0);
        return;
    }

    php_kislay_promise_t *promise = php_kislay_promise_from_obj(Z_OBJ_P(getThis()));
    if (promise->owner_app != nullptr) {
        kislay_async_drain_lane(promise->owner_app, promise->owner_lane, 64);
    }
    KislayAsyncLaneState *lane_state = promise->owner_app != nullptr
        ? kislay_async_lane_state(promise->owner_app, promise->owner_lane)
        : nullptr;
    if (promise->state == PromiseState::Pending && lane_state != nullptr && lane_state->promise_registry) {
        lane_state->promise_registry->add_fulfilled_callback(promise->async_id, on_fulfilled);
        if (on_rejected) {
            lane_state->promise_registry->add_rejected_callback(promise->async_id, on_rejected);
        }
    } else {
        if (promise->state == PromiseState::Fulfilled) {
            kislay_promise_invoke_callback(on_fulfilled, promise);
        } else if (promise->state == PromiseState::Rejected && on_rejected != nullptr) {
            kislay_promise_invoke_callback(on_rejected, promise);
        }
    }

    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayPromise, catch) {
    zval *on_rejected = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(on_rejected)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(on_rejected)) {
        zend_throw_exception(zend_ce_exception, "onRejected must be callable", 0);
        return;
    }

    php_kislay_promise_t *promise = php_kislay_promise_from_obj(Z_OBJ_P(getThis()));
    if (promise->owner_app != nullptr) {
        kislay_async_drain_lane(promise->owner_app, promise->owner_lane, 64);
    }
    KislayAsyncLaneState *lane_state = promise->owner_app != nullptr
        ? kislay_async_lane_state(promise->owner_app, promise->owner_lane)
        : nullptr;
    if (promise->state == PromiseState::Pending && lane_state != nullptr && lane_state->promise_registry) {
        lane_state->promise_registry->add_rejected_callback(promise->async_id, on_rejected);
    } else {
        if (promise->state == PromiseState::Rejected) {
            kislay_promise_invoke_callback(on_rejected, promise);
        }
    }

    RETURN_ZVAL(getThis(), 1, 0);
}

PHP_METHOD(KislayPromise, finally) {
    zval *on_finally = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(on_finally)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(on_finally)) {
        zend_throw_exception(zend_ce_exception, "onFinally must be callable", 0);
        return;
    }

    // Simplify finally as then(f, f)
    php_kislay_promise_t *promise = php_kislay_promise_from_obj(Z_OBJ_P(getThis()));
    if (promise->owner_app != nullptr) {
        kislay_async_drain_lane(promise->owner_app, promise->owner_lane, 64);
    }
    KislayAsyncLaneState *lane_state = promise->owner_app != nullptr
        ? kislay_async_lane_state(promise->owner_app, promise->owner_lane)
        : nullptr;
    if (promise->state == PromiseState::Pending && lane_state != nullptr && lane_state->promise_registry) {
        lane_state->promise_registry->add_finally_callback(promise->async_id, on_finally);
    } else {
        kislay_promise_invoke_callback(on_finally, promise);
    }

    RETURN_ZVAL(getThis(), 1, 0);
}

static void kislay_promise_dispatch(php_kislay_promise_t *promise) {
    if (promise == nullptr || promise->state == PromiseState::Pending) {
        return;
    }
    KislayAsyncLaneState *lane_state = promise->owner_app != nullptr
        ? kislay_async_lane_state(promise->owner_app, promise->owner_lane)
        : nullptr;
    if (lane_state != nullptr && lane_state->promise_registry) {
        lane_state->promise_registry->dispatch(promise);
    }
}

static void kislay_promise_invoke_callback(zval *callback, php_kislay_promise_t *promise) {
    if (callback == nullptr || promise == nullptr || promise->state == PromiseState::Pending) {
        return;
    }

    zval args[1];
    zval retval;
    ZVAL_COPY(&args[0], &promise->result);
    if (kislay_call_php(callback, 1, args, &retval)) {
        zval_ptr_dtor(&retval);
    }
    zval_ptr_dtor(&args[0]);
}

static kislay::runtime::TaskId kislay_next_async_id(php_kislay_app_t *app) {
    return app->next_async_id.fetch_add(1, std::memory_order_relaxed);
}

static bool kislay_async_has_pending_for_request(php_kislay_app_t *app,
                                                 const std::string &request_id,
                                                 std::size_t lane_index) {
    if (app == nullptr || request_id.empty()) {
        return false;
    }
    KislayAsyncLaneState *lane_state = kislay_async_lane_state(app, lane_index);
    if (lane_state == nullptr) {
        return false;
    }
    auto it = lane_state->pending_request_counts.find(request_id);
    return it != lane_state->pending_request_counts.end() && it->second > 0;
}

static bool kislay_is_one_shot_task_complete(const php_kislay_app_t *app, kislay::runtime::PhpTaskId task_id) {
    if (app == nullptr || app->scheduler_lock == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(*app->scheduler_lock);
    for (const auto &task : app->scheduled_tasks) {
        if (task.task_id == task_id) {
            return task.type == kislay_scheduled_task::ONCE && task.fired;
        }
    }
    return false;
}

static void kislay_async_resolve_http_result(php_kislay_app_t *app, kislay::runtime::HttpResultMessage &result) {
    KislayAsyncLaneState *lane_state = kislay_async_lane_state(app, result.owner_lane);
    if (lane_state == nullptr) {
        return;
    }
    auto it = lane_state->pending_http_tasks.find(result.task_id);
    if (it == lane_state->pending_http_tasks.end()) {
        return;
    }

    KislayPendingHttpTask pending = it->second;
    lane_state->pending_http_tasks.erase(it);
    kislay_async_untrack_request(lane_state, pending.request_id);
    php_kislay_promise_t *promise = lane_state->promise_registry ? lane_state->promise_registry->get_promise(result.task_id) : nullptr;
    if (promise == nullptr) {
        OBJ_RELEASE(&pending.async_http->std);
        return;
    }

    pending.async_http->response_body = result.response_body;
    pending.async_http->response_code = result.response_code;

    zval settle_value;
    if (result.ok) {
        ZVAL_TRUE(&settle_value);
        kislay_promise_resolve(promise, &settle_value);
    } else {
        ZVAL_STRING(&settle_value, result.error_message.c_str());
        kislay_promise_reject(promise, &settle_value);
    }
    zval_ptr_dtor(&settle_value);
    kislay_promise_dispatch(promise);
    if (lane_state->promise_registry) {
        lane_state->promise_registry->unregister_promise(result.task_id);
    }

    OBJ_RELEASE(&pending.async_http->std);
}

static void kislay_async_run_php_task(php_kislay_app_t *app,
                                      std::size_t lane_index,
                                      kislay::runtime::PhpTaskId task_id) {
    KislayAsyncLaneState *lane_state = kislay_async_lane_state(app, lane_index);
    if (lane_state == nullptr) {
        return;
    }

    auto deferred = lane_state->pending_php_tasks.find(task_id);
    if (deferred != lane_state->pending_php_tasks.end()) {
        KislayPendingPhpTask task = deferred->second;
        lane_state->pending_php_tasks.erase(deferred);
        kislay_async_untrack_request(lane_state, task.request_id);
        php_kislay_promise_t *promise = lane_state->promise_registry ? lane_state->promise_registry->get_promise(task_id) : nullptr;
        if (promise == nullptr) {
            zval_ptr_dtor(&task.callable);
            return;
        }

        zval retval;
        if (kislay_call_php(&task.callable, 0, nullptr, &retval)) {
            if (EG(exception)) {
                zval ex_obj;
                ZVAL_OBJ(&ex_obj, EG(exception));
                GC_ADDREF(EG(exception));
                zend_clear_exception();
                kislay_promise_reject(promise, &ex_obj);
                zval_ptr_dtor(&ex_obj);
            } else {
                kislay_promise_resolve(promise, &retval);
                zval_ptr_dtor(&retval);
            }
        } else {
            zval error;
            ZVAL_STRING(&error, "Deferred PHP task failed");
            kislay_promise_reject(promise, &error);
            zval_ptr_dtor(&error);
        }

        zval_ptr_dtor(&task.callable);
        kislay_promise_dispatch(promise);
        if (lane_state->promise_registry) {
            lane_state->promise_registry->unregister_promise(task_id);
        }
        return;
    }

    auto callback_it = app->scheduled_callbacks.find(task_id);
    if (callback_it == app->scheduled_callbacks.end()) {
        return;
    }

    zval retval;
    if (kislay_call_php(&callback_it->second, 0, nullptr, &retval)) {
        zval_ptr_dtor(&retval);
    }

    if (kislay_is_one_shot_task_complete(app, task_id)) {
        zval_ptr_dtor(&callback_it->second);
        app->scheduled_callbacks.erase(callback_it);
    }
}

static void kislay_async_drain_lane(php_kislay_app_t *app, std::size_t lane_index, std::size_t budget) {
    if (app == nullptr || app->async_bridge == nullptr) {
        return;
    }
    ZEND_ASSERT(kislay_php_thread_active || EG(current_execute_data) != nullptr);

    std::size_t processed = 0;
    while (processed < budget) {
        bool had_work = false;

        kislay::runtime::PhpTaskMessage php_task;
        while (processed < budget && app->async_bridge->try_pop_php_task(lane_index, php_task)) {
            had_work = true;
            ++processed;
            kislay_async_run_php_task(app, php_task.owner_lane, php_task.task_id);
        }

        kislay::runtime::HttpResultMessage result;
        while (processed < budget && app->async_bridge->try_pop_result(lane_index, result)) {
            had_work = true;
            ++processed;
            kislay_async_resolve_http_result(app, result);
        }

        if (!had_work) {
            break;
        }
    }
}

static void kislay_async_drain(php_kislay_app_t *app, std::size_t budget) {
    kislay_async_drain_lane(app, kislay_async_lane_index_for_thread(app), budget);
}

static void kislay_async_wait_for_request(php_kislay_app_t *app, const std::string &request_id, zend_long timeout_ms) {
    if (app == nullptr || request_id.empty()) {
        return;
    }
    const std::size_t lane_index = kislay_async_lane_index_for_thread(app);

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(timeout_ms >= 0 ? timeout_ms : 10000);

    while (kislay_async_has_pending_for_request(app, request_id, lane_index)) {
        kislay_async_drain(app, 128);
        if (!kislay_async_has_pending_for_request(app, request_id, lane_index)) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - start >= timeout) {
            break;
        }

        if (app->async_bridge != nullptr) {
            app->async_bridge->wait_for_activity_for(lane_index, std::chrono::milliseconds(10));
        } else {
            // Sleep until the next scheduled task deadline
        {
            long long sleep_ms = 1000LL;
            long long now_check = kislay_now_ms();
            std::lock_guard<std::mutex> lk(*app->scheduler_lock);
            for (auto &t : app->scheduled_tasks) {
                if (t.type == kislay_scheduled_task::ONCE && t.fired) continue;
                long long remaining = t.next_run_ms - now_check;
                if (remaining > 0 && remaining < sleep_ms) sleep_ms = remaining;
            }
            sleep_ms = sleep_ms < 1 ? 1 : sleep_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        }
    }
}

PHP_FUNCTION(async) {
    zval *task_callable = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(task_callable)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(task_callable)) {
        zend_throw_exception(zend_ce_exception, "Task must be callable", 0);
        return;
    }

    php_kislay_app_t *app = kislay_active_app.load(std::memory_order_relaxed);
    const std::size_t lane_index = kislay_async_lane_index_for_thread(app);
    KislayAsyncLaneState *lane_state = kislay_async_lane_state(app, lane_index);
    if (!app) {
        zend_throw_exception(zend_ce_exception, "No active Kislay App found for async operation", 0);
        return;
    }

    object_init_ex(return_value, kislay_promise_ce);
    php_kislay_promise_t *promise = php_kislay_promise_from_obj(Z_OBJ_P(return_value));
    promise->owner_app = app;
    promise->async_id = kislay_next_async_id(app);
    promise->owner_lane = lane_index;
    if (kislay_active_request != nullptr) {
        promise->owner_request_id = kislay_active_request->request_id;
    }
    if (lane_state != nullptr && lane_state->promise_registry) {
        lane_state->promise_registry->register_promise(promise->async_id, promise);
    }

    KislayPendingPhpTask task;
    ZVAL_COPY(&task.callable, task_callable);
    task.request_id = promise->owner_request_id;
    if (lane_state == nullptr) {
        zval_ptr_dtor(&task.callable);
        zend_throw_exception(zend_ce_exception, "Async PHP lane state is not initialized", 0);
        RETURN_NULL();
    }
    auto [pending_it, inserted] = lane_state->pending_php_tasks.emplace(promise->async_id, std::move(task));
    if (inserted) {
        kislay_async_track_request(lane_state, pending_it->second.request_id);
    }

    if (!app->loop_active.load(std::memory_order_relaxed)) {
        app->loop_active.store(true, std::memory_order_relaxed);
    }
    if (app->async_bridge && !app->async_bridge->running()) {
        app->async_bridge->start(
            static_cast<std::size_t>(std::max(app->async_worker_count, 1)),
            std::max(app->php_runtime_threads, std::size_t{1})
        );
    }

    if (app->async_bridge == nullptr || !app->async_bridge->schedule_php_task(lane_index, promise->async_id)) {
        auto pending_it = lane_state->pending_php_tasks.find(promise->async_id);
        if (pending_it != lane_state->pending_php_tasks.end()) {
            kislay_async_untrack_request(lane_state, pending_it->second.request_id);
            zval_ptr_dtor(&pending_it->second.callable);
            lane_state->pending_php_tasks.erase(pending_it);
        }
        if (lane_state->promise_registry) {
            lane_state->promise_registry->unregister_promise(promise->async_id);
        }
        zend_throw_exception(zend_ce_exception, "Async PHP queue is overloaded", 0);
        RETURN_NULL();
    }
}

/* ---- ServiceClient helpers ---- */

static size_t kislay_service_client_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    std::string *buf = static_cast<std::string *>(userdata);
    buf->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t kislay_service_client_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    std::map<std::string, std::string> *headers = static_cast<std::map<std::string, std::string> *>(userdata);
    std::string line(buffer, size * nitems);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
        for (auto &c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        (*headers)[name] = value;
    }
    return size * nitems;
}

static std::string kislay_service_client_expand_path(const std::string &path,
                                                      zval *params_zval,
                                                      std::string &query_out) {
    std::string result = path;
    std::map<std::string, std::string> leftover;

    if (params_zval && Z_TYPE_P(params_zval) == IS_ARRAY) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(params_zval), key, val) {
            if (!key) continue;
            std::string k(ZSTR_VAL(key), ZSTR_LEN(key));
            std::string placeholder = "{" + k + "}";
            zval tmp;
            ZVAL_COPY(&tmp, val);
            convert_to_string(&tmp);
            std::string v(Z_STRVAL(tmp), Z_STRLEN(tmp));
            zval_ptr_dtor(&tmp);
            auto pos = result.find(placeholder);
            if (pos != std::string::npos) {
                result.replace(pos, placeholder.size(), v);
            } else {
                leftover[k] = v;
            }
        } ZEND_HASH_FOREACH_END();
    }

    if (!leftover.empty()) {
        std::string qs;
        for (auto &kv : leftover) {
            if (!qs.empty()) qs += "&";
            qs += kv.first + "=" + kv.second;
        }
        query_out = qs;
    }
    return result;
}

static bool kislay_service_client_do_request(php_kislay_service_client_t *obj,
                                              const std::string &method,
                                              const std::string &url,
                                              const std::string &body_json,
                                              long &response_status,
                                              std::map<std::string, std::string> &response_headers,
                                              std::string &response_body,
                                              std::string &error_msg) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        error_msg = "Failed to init curl";
        return false;
    }

    struct curl_slist *headers_list = nullptr;
    pthread_mutex_lock(&obj->lock);
    for (auto &hdr : obj->default_headers) {
        std::string h = hdr.first + ": " + hdr.second;
        headers_list = curl_slist_append(headers_list, h.c_str());
    }
    long timeout = obj->timeout_ms;
    pthread_mutex_unlock(&obj->lock);

    if (method == "POST" || method == "PUT" || method == "PATCH") {
        headers_list = curl_slist_append(headers_list, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, kislay_service_client_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, kislay_service_client_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    if (!body_json.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_json.size());
    }

    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
        ok = true;
    } else {
        error_msg = std::string("curl error: ") + curl_easy_strerror(res) + " url=" + url;
    }

    if (headers_list) curl_slist_free_all(headers_list);
    curl_easy_cleanup(curl);
    return ok;
}

static void kislay_service_client_request(php_kislay_service_client_t *obj,
                                           const std::string &method,
                                           zend_string *path_str,
                                           zval *params_zval,
                                           zval *body_zval,
                                           zval *return_value) {
    std::string query_str;
    std::string expanded = kislay_service_client_expand_path(
        std::string(ZSTR_VAL(path_str), ZSTR_LEN(path_str)),
        params_zval, query_str);

    pthread_mutex_lock(&obj->lock);
    std::string base = obj->base_url;
    int retries = obj->retry_count;
    int retry_delay = obj->retry_delay_ms;
    pthread_mutex_unlock(&obj->lock);

    std::string url = base + expanded;
    if (!query_str.empty()) {
        url += (url.find('?') == std::string::npos ? "?" : "&") + query_str;
    }

    std::string body_json;
    if (body_zval && Z_TYPE_P(body_zval) == IS_ARRAY) {
        smart_str buf = {nullptr, 0};
        php_json_encode(&buf, body_zval, 0);
        smart_str_0(&buf);
        if (buf.s) {
            body_json = std::string(ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
            smart_str_free(&buf);
        }
    }

    long status = 0;
    std::map<std::string, std::string> resp_headers;
    std::string resp_body;
    std::string error_msg;

    bool success = false;
    for (int attempt = 0; attempt <= retries; attempt++) {
        resp_headers.clear();
        resp_body.clear();
        status = 0;
        success = kislay_service_client_do_request(obj, method, url, body_json,
                                                    status, resp_headers, resp_body, error_msg);
        if (!success) break;
        if (status < 500) break;
        if (attempt < retries) {
#ifndef _WIN32
            struct timespec ts;
            ts.tv_sec = retry_delay / 1000;
            ts.tv_nsec = (retry_delay % 1000) * 1000000L;
            nanosleep(&ts, nullptr);
#else
            Sleep(retry_delay);
#endif
        }
    }

    if (!success) {
        zend_throw_exception_ex(zend_ce_exception, 0, "ServiceClient request failed: %s", error_msg.c_str());
        return;
    }

    array_init(return_value);
    add_assoc_long(return_value, "status", (zend_long)status);
    add_assoc_bool(return_value, "ok", status < 400 ? 1 : 0);

    zval headers_zval;
    array_init(&headers_zval);
    for (auto &h : resp_headers) {
        add_assoc_string(&headers_zval, h.first.c_str(), h.second.c_str());
    }
    add_assoc_zval(return_value, "headers", &headers_zval);

    zval body_decoded;
    ZVAL_NULL(&body_decoded);
    if (!resp_body.empty()) {
        zend_string *body_zstr = zend_string_init(resp_body.c_str(), resp_body.size(), 0);
        php_json_decode_ex(&body_decoded, ZSTR_VAL(body_zstr), ZSTR_LEN(body_zstr),
                           PHP_JSON_OBJECT_AS_ARRAY, 512);
        zend_string_release(body_zstr);
        if (Z_TYPE(body_decoded) == IS_NULL) {
            zval_ptr_dtor(&body_decoded);
            ZVAL_STRINGL(&body_decoded, resp_body.c_str(), resp_body.size());
        }
    }
    add_assoc_zval(return_value, "body", &body_decoded);
}

/* ---- ServiceClient arginfos ---- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislayphp_sc_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, serviceNameOrUrl, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_sc_get, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, params, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_kislayphp_sc_post, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, body, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_kislayphp_sc_with_header, 0, 2, KislayServiceClient, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_kislayphp_sc_with_timeout, 0, 1, KislayServiceClient, 0)
    ZEND_ARG_TYPE_INFO(0, ms, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_kislayphp_sc_with_retry, 0, 1, KislayServiceClient, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, delayMs, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_kislayphp_sc_with_base_url, 0, 1, KislayServiceClient, 0)
    ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_kislayphp_sc_from_discovery, 0, 1, KislayServiceClient, 0)
    ZEND_ARG_TYPE_INFO(0, serviceName, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ---- ServiceClient PHP_METHODs ---- */

PHP_METHOD(KislayServiceClient, __construct) {
    zend_string *name_or_url = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name_or_url)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    std::string input(ZSTR_VAL(name_or_url), ZSTR_LEN(name_or_url));

    if (input.rfind("http", 0) == 0) {
        obj->base_url = input;
    } else {
        obj->service_name = input;
        zend_class_entry *discovery_ce = static_cast<zend_class_entry *>(
            zend_hash_str_find_ptr(CG(class_table), "kislayDiscovery", sizeof("kislayDiscovery") - 1));
        if (discovery_ce) {
            zval retval, zname;
            ZVAL_UNDEF(&retval);
            ZVAL_STR(&zname, name_or_url);
            if (kislay_call_static_method_one_param(discovery_ce, "resolve", &zname, &retval) &&
                Z_TYPE(retval) == IS_STRING) {
                obj->base_url = std::string(Z_STRVAL(retval), Z_STRLEN(retval));
            }
            zval_ptr_dtor(&retval);
        }
    }
}

PHP_METHOD(KislayServiceClient, get) {
    zend_string *path = nullptr;
    zval *params = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(params)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    kislay_service_client_request(obj, "GET", path, params, nullptr, return_value);
}

PHP_METHOD(KislayServiceClient, post) {
    zend_string *path = nullptr;
    zval *body = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(body)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    kislay_service_client_request(obj, "POST", path, nullptr, body, return_value);
}

PHP_METHOD(KislayServiceClient, put) {
    zend_string *path = nullptr;
    zval *body = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(body)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    kislay_service_client_request(obj, "PUT", path, nullptr, body, return_value);
}

PHP_METHOD(KislayServiceClient, delete_) {
    zend_string *path = nullptr;
    zval *params = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(params)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    kislay_service_client_request(obj, "DELETE", path, params, nullptr, return_value);
}

PHP_METHOD(KislayServiceClient, patch) {
    zend_string *path = nullptr;
    zval *body = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(body)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    kislay_service_client_request(obj, "PATCH", path, nullptr, body, return_value);
}

PHP_METHOD(KislayServiceClient, withHeader) {
    zend_string *name = nullptr, *value = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    pthread_mutex_lock(&obj->lock);
    obj->default_headers[std::string(ZSTR_VAL(name), ZSTR_LEN(name))] =
        std::string(ZSTR_VAL(value), ZSTR_LEN(value));
    pthread_mutex_unlock(&obj->lock);
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(KislayServiceClient, withTimeout) {
    zend_long ms = 5000;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    pthread_mutex_lock(&obj->lock);
    obj->timeout_ms = (long)ms;
    pthread_mutex_unlock(&obj->lock);
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(KislayServiceClient, withRetry) {
    zend_long count = 0, delay_ms = 100;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_LONG(count)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(delay_ms)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    pthread_mutex_lock(&obj->lock);
    obj->retry_count = (int)count;
    obj->retry_delay_ms = (int)delay_ms;
    pthread_mutex_unlock(&obj->lock);
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(KislayServiceClient, withBaseUrl) {
    zend_string *url = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(url)
    ZEND_PARSE_PARAMETERS_END();
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(ZEND_THIS));
    pthread_mutex_lock(&obj->lock);
    obj->base_url = std::string(ZSTR_VAL(url), ZSTR_LEN(url));
    pthread_mutex_unlock(&obj->lock);
    RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(KislayServiceClient, fromDiscovery) {
    zend_string *service_name = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(service_name)
    ZEND_PARSE_PARAMETERS_END();

    zend_class_entry *discovery_ce = static_cast<zend_class_entry *>(zend_hash_str_find_ptr(
        CG(class_table), "kislaydiscovery", sizeof("kislaydiscovery") - 1));
    if (!discovery_ce) {
        zend_throw_exception_ex(zend_ce_exception, 0,
            "KislayDiscovery extension not loaded; cannot resolve service '%s'",
            ZSTR_VAL(service_name));
        return;
    }

    zval retval, zname;
    ZVAL_UNDEF(&retval);
    ZVAL_STR(&zname, service_name);
    if (!kislay_call_static_method_one_param(discovery_ce, "resolve", &zname, &retval) ||
        Z_TYPE(retval) != IS_STRING) {
        zval_ptr_dtor(&retval);
        zend_throw_exception_ex(zend_ce_exception, 0,
            "KislayDiscovery::resolve() did not return a URL for service '%s'",
            ZSTR_VAL(service_name));
        return;
    }

    object_init_ex(return_value, kislay_service_client_ce);
    php_kislay_service_client_t *obj = php_kislay_service_client_from_obj(Z_OBJ_P(return_value));
    obj->service_name = std::string(ZSTR_VAL(service_name), ZSTR_LEN(service_name));
    obj->base_url = std::string(Z_STRVAL(retval), Z_STRLEN(retval));
    zval_ptr_dtor(&retval);
}

static const zend_function_entry kislay_service_client_methods[] = {
    PHP_ME(KislayServiceClient, __construct, arginfo_kislayphp_sc_construct, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, get, arginfo_kislayphp_sc_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, post, arginfo_kislayphp_sc_post, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, put, arginfo_kislayphp_sc_post, ZEND_ACC_PUBLIC)
    PHP_MALIAS(KislayServiceClient, delete, delete_, arginfo_kislayphp_sc_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, patch, arginfo_kislayphp_sc_post, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, withHeader, arginfo_kislayphp_sc_with_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, withTimeout, arginfo_kislayphp_sc_with_timeout, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, withRetry, arginfo_kislayphp_sc_with_retry, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, withBaseUrl, arginfo_kislayphp_sc_with_base_url, ZEND_ACC_PUBLIC)
    PHP_ME(KislayServiceClient, fromDiscovery, arginfo_kislayphp_sc_from_discovery, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry kislay_promise_methods[] = {
    PHP_ME(KislayPromise, then, arginfo_kislay_promise_then, ZEND_ACC_PUBLIC)
    PHP_ME(KislayPromise, catch, arginfo_kislay_promise_catch, ZEND_ACC_PUBLIC)
    PHP_ME(KislayPromise, finally, arginfo_kislay_promise_finally, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry kislay_functions[] = {
    PHP_FE(async, arginfo_kislay_async)
    PHP_FE_END
};

static const zend_function_entry kislay_request_methods[] = {
    PHP_ME(KislayRequest, getMethod, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, method, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getUri, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getPath, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, path, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getQuery, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, query, arginfo_kislay_request_query, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getQueryParams, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getBody, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, body, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getParams, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, param, arginfo_kislay_request_input, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getHeaders, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getHeader, arginfo_kislay_request_get_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, header, arginfo_kislay_request_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, hasHeader, arginfo_kislay_request_has, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, input, arginfo_kislay_request_input, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, json, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getJson, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, isJson, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, all, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, has, arginfo_kislay_request_has, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, only, arginfo_kislay_request_only, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, setAttribute, arginfo_kislay_request_set_attribute, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getAttribute, arginfo_kislay_request_get_attribute, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, hasAttribute, arginfo_kislay_request_has, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, user,       arginfo_kislay_void,               ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, hasRole,    arginfo_kislay_req_has_role,       ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, traceId,    arginfo_kislayphp_req_trace_id,    ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, spanId,     arginfo_kislayphp_req_span_id,     ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, traceparent,arginfo_kislayphp_req_traceparent, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, tracestate, arginfo_kislayphp_req_tracestate,  ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, id, arginfo_kislay_req_id, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry kislay_response_methods[] = {
    PHP_ME(KislayResponse, setBody, arginfo_kislay_response_set_body, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, setStatusCode, arginfo_kislay_response_set_status, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, setHeader, arginfo_kislay_response_set_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, header, arginfo_kislay_response_set_header_alias, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, set, arginfo_kislay_response_set_header_alias, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, type, arginfo_kislay_response_type, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, status, arginfo_kislay_response_status, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendStatus, arginfo_kislay_response_status, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, send, arginfo_kislay_response_send, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendJson, arginfo_kislay_response_send_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendXml, arginfo_kislay_response_send_xml, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendFile, arginfo_kislay_response_send_file, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, json, arginfo_kislay_response_send_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, xml, arginfo_kislay_response_send_xml, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, ok, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, created, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, noContent, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, badRequest, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, unauthorized, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, forbidden, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, notFound, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, methodNotAllowed, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, conflict, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, unprocessableEntity, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, internalServerError, arginfo_kislay_request_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, getBody, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, getStatusCode, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry kislay_app_methods[] = {
    PHP_ME(KislayApp, __construct, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, setOption, arginfo_kislay_app_set_option, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, use, arginfo_kislay_app_use, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, get, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, post, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, put, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, patch, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, delete, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, options, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, all, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, onRequestStart, arginfo_kislay_app_request_hook, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, onRequestEnd, arginfo_kislay_app_request_hook, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, group, arginfo_kislay_app_group, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, setMemoryLimit, arginfo_kislay_app_set_memory_limit, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, getMemoryLimit, arginfo_kislay_app_get_memory_limit, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, enableGc, arginfo_kislay_app_enable_gc, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, enableAsync, arginfo_kislay_app_enable_async, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, listen, arginfo_kislay_app_listen, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, listenAsync, arginfo_kislay_app_listen, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, wait, arginfo_kislay_app_wait, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, isRunning, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, stop, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, onNotFound,          arginfo_kislay_app_on_not_found,    ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, onError,             arginfo_kislay_app_on_not_found,    ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, addHealthIndicator,  arginfo_kislay_app_request_hook,    ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, every, arginfo_kislay_app_every, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, once, arginfo_kislay_app_once, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, schedule, arginfo_kislay_app_schedule, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, mount, arginfo_kislay_app_mount, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry kislay_async_http_methods[] = {
    PHP_ME(KislayAsyncHttp, __construct, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, get, arginfo_kislay_async_http_request, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, post, arginfo_kislay_async_http_request, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, put, arginfo_kislay_async_http_request, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, patch, arginfo_kislay_async_http_request, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, delete, arginfo_kislay_async_http_request, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, setHeader, arginfo_kislay_async_http_set_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, retry, arginfo_kislay_async_http_retry, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, execute, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, executeAsync, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, getResponse, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, getResponseCode, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

// ── JWT helpers ───────────────────────────────────────────────────────────────
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

static void kislay_base64url_decode(const std::string &in, std::string &out) {
    std::string b64 = in;
    for (auto &c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64 += '=';
    BIO *bio = BIO_new_mem_buf(b64.data(), (int)b64.size());
    BIO *b64bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64bio, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64bio, bio);
    char buf[4096]; int len;
    while ((len = BIO_read(bio, buf, sizeof(buf))) > 0) out.append(buf, len);
    BIO_free_all(bio);
}

static bool kislay_jwt_verify_hs256(const std::string &header_b64, const std::string &payload_b64,
                                     const std::string &sig_b64, const std::string &secret) {
    std::string signing_input = header_b64 + "." + payload_b64;
    unsigned char digest[32]; unsigned int dlen = 32;
    HMAC(EVP_sha256(), secret.data(), (int)secret.size(),
         (unsigned char*)signing_input.data(), signing_input.size(), digest, &dlen);
    BIO *bmem = BIO_new(BIO_s_mem()), *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, digest, dlen); BIO_flush(b64);
    BUF_MEM *bptr; BIO_get_mem_ptr(b64, &bptr);
    std::string computed(bptr->data, bptr->length);
    BIO_free_all(b64);
    for (auto &c : computed) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!computed.empty() && computed.back() == '=') computed.pop_back();
    if (computed.size() != sig_b64.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < computed.size(); i++) diff |= (unsigned char)(computed[i] ^ sig_b64[i]);
    return diff == 0;
}

static bool kislay_jwt_parse_payload(const std::string &payload_b64, zval *out) {
    std::string decoded;
    kislay_base64url_decode(payload_b64, decoded);
    ZVAL_NULL(out);
    return php_json_decode_ex(out, decoded.c_str(), (int)decoded.size(), PHP_JSON_OBJECT_AS_ARRAY, 512) == SUCCESS;
}

// ── Scheduler thread ───────────────────────────────────────────────────────────

// ── Cron expression parser ─────────────────────────────────────────────────────
static bool kislay_cron_field_matches(const std::string &field, int value, int min_val, int max_val) {
    if (field == "*") return true;
    if (field.find('/') != std::string::npos) {
        int step = 1;
        try { step = std::stoi(field.substr(field.find('/') + 1)); } catch (...) { return false; }
        return step > 0 && ((value - min_val) % step == 0);
    }
    if (field.find('-') != std::string::npos) {
        auto dash = field.find('-');
        try {
            int lo = std::stoi(field.substr(0, dash));
            int hi = std::stoi(field.substr(dash + 1));
            return value >= lo && value <= hi;
        } catch (...) { return false; }
    }
    if (field.find(',') != std::string::npos) {
        std::stringstream ss(field);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try { if (std::stoi(token) == value) return true; } catch (...) {}
        }
        return false;
    }
    try { return std::stoi(field) == value; } catch (...) { return false; }
}

static long long kislay_cron_next_ms(const std::string &expr, long long from_ms) {
    std::vector<std::string> fields;
    std::stringstream ss(expr);
    std::string f;
    while (ss >> f) fields.push_back(f);
    if (fields.size() < 5) return from_ms + 60000LL;

    // Start from next minute boundary
    time_t from_sec = (time_t)(from_ms / 1000) + 60;
    from_sec -= from_sec % 60;

    // Scan forward up to 1 year (525600 minutes)
    for (int i = 0; i < 525600; i++) {
        time_t t = from_sec + (time_t)(i * 60);
        struct tm tm_val;
#ifdef _WIN32
        gmtime_s(&tm_val, &t);
        struct tm *tm_ptr = &tm_val;
#else
        struct tm *tm_ptr = gmtime_r(&t, &tm_val);
#endif
        if (!tm_ptr) break;
        if (kislay_cron_field_matches(fields[0], tm_ptr->tm_min,  0, 59) &&
            kislay_cron_field_matches(fields[1], tm_ptr->tm_hour, 0, 23) &&
            kislay_cron_field_matches(fields[2], tm_ptr->tm_mday, 1, 31) &&
            kislay_cron_field_matches(fields[3], tm_ptr->tm_mon + 1, 1, 12) &&
            kislay_cron_field_matches(fields[4], tm_ptr->tm_wday, 0, 6)) {
            return (long long)t * 1000LL;
        }
    }
    return from_ms + 60000LL;
}

static void kislay_run_scheduler(php_kislay_app_t *obj) {
    while (obj->scheduler_running.load(std::memory_order_relaxed)) {
        long long now = kislay_now_ms();
        {
            std::lock_guard<std::mutex> lock(*obj->scheduler_lock);
            for (auto &task : obj->scheduled_tasks) {
            if (task.type == kislay_scheduled_task::ONCE && task.fired) continue;
            if (now >= task.next_run_ms) {
                const bool queued = obj->async_bridge != nullptr &&
                    obj->async_bridge->schedule_php_task(0, task.task_id);
                if (!queued) {
                    continue;
                }
                if (task.type == kislay_scheduled_task::ONCE) {
                    task.fired = true;
                } else if (task.type == kislay_scheduled_task::INTERVAL) {
                    task.next_run_ms = now + task.interval_ms;
                } else if (task.type == kislay_scheduled_task::CRON) {
                    task.next_run_ms = kislay_cron_next_ms(task.cron, kislay_now_ms());
                } else {
                    task.next_run_ms = now + task.interval_ms;
                }
            }
        }
        }
        // Sleep until the next scheduled task deadline
        {
            long long sleep_ms = 1000LL;
            long long now_check = kislay_now_ms();
            std::lock_guard<std::mutex> lk(*obj->scheduler_lock);
            for (auto &t : obj->scheduled_tasks) {
                if (t.type == kislay_scheduled_task::ONCE && t.fired) continue;
                long long remaining = t.next_run_ms - now_check;
                if (remaining > 0 && remaining < sleep_ms) sleep_ms = remaining;
            }
            sleep_ms = sleep_ms < 1 ? 1 : sleep_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
}

// ── Controller syntax helpers ──────────────────────────────────────────────────
static bool kislay_is_controller_handler(zval *handler) {
    if (Z_TYPE_P(handler) == IS_STRING) {
        zend_string *s = Z_STR_P(handler);
        return (memchr(ZSTR_VAL(s), '@', ZSTR_LEN(s)) != nullptr);
    }
    if (Z_TYPE_P(handler) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(handler)) == 2) {
        zval *first = zend_hash_index_find(Z_ARRVAL_P(handler), 0);
        return first && Z_TYPE_P(first) == IS_STRING;
    }
    return false;
}

static bool kislay_normalize_controller_callable(zval *handler, zval *out) {
    if (Z_TYPE_P(handler) == IS_STRING) {
        const char *at = strchr(Z_STRVAL_P(handler), '@');
        if (!at) return false;
        std::string class_name(Z_STRVAL_P(handler), at - Z_STRVAL_P(handler));
        std::string method_name(at + 1);
        zend_string *cls_str = zend_string_init(class_name.c_str(), class_name.size(), 0);
        zend_class_entry *ce = zend_lookup_class(cls_str);
        zend_string_release(cls_str);
        if (!ce) return false;
        zval instance; object_init_ex(&instance, ce);
        kislay_call_object_method_no_args(&instance, "__construct");
        array_init(out);
        add_next_index_zval(out, &instance);
        add_next_index_string(out, method_name.c_str());
        return true;
    }
    if (Z_TYPE_P(handler) == IS_ARRAY) {
        zval *class_zv = zend_hash_index_find(Z_ARRVAL_P(handler), 0);
        zval *method_zv = zend_hash_index_find(Z_ARRVAL_P(handler), 1);
        if (!class_zv || !method_zv || Z_TYPE_P(class_zv) != IS_STRING || Z_TYPE_P(method_zv) != IS_STRING) return false;
        zend_class_entry *ce = zend_lookup_class(Z_STR_P(class_zv));
        if (!ce) return false;
        zval instance; object_init_ex(&instance, ce);
        kislay_call_object_method_no_args(&instance, "__construct");
        array_init(out);
        add_next_index_zval(out, &instance);
        add_next_index_zval(out, method_zv);
        return true;
    }
    return false;
}

// ── Sub-app dispatch ───────────────────────────────────────────────────────────
static bool kislay_dispatch_to_subapp(zval *subapp_obj, const std::string &method, const std::string &stripped_uri, zval *req_obj, zval *res_obj) {
    if (Z_TYPE_P(subapp_obj) != IS_OBJECT) return false;
    php_kislay_app_t *sub = (php_kislay_app_t*)((char*)Z_OBJ_P(subapp_obj) - XtOffsetOf(php_kislay_app_t, std));
    if (!sub) return false;
    // Update req path to stripped URI
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(req_obj));
    std::string original_path = req->path;
    req->path = stripped_uri.empty() ? "/" : stripped_uri;
    bool handled = false;
    thread_local std::vector<std::string_view> path_segments;
    kislay_collect_path_segments(req->path, path_segments);
    if (auto *route = kislay_find_route(sub, method, req->path, path_segments, req)) {
        if (!route->compiled_middleware.empty()) {
            if (!kislay_run_middleware_list(route->compiled_middleware, req_obj, res_obj, nullptr)) {
                req->path = original_path;
                return false;
            }
        }
        zval args[2];
        ZVAL_COPY_VALUE(&args[0], req_obj);
        ZVAL_COPY_VALUE(&args[1], res_obj);
        zval retval;
        ZVAL_UNDEF(&retval);
        handled = kislay_call_php(&route->handler, 2, args, &retval, nullptr);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
    }
    req->path = original_path;
    return handled;
}

PHP_MINIT_FUNCTION(kislayphp_extension) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    REGISTER_INI_ENTRIES();
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "Request", kislay_request_methods);
    kislay_request_ce = zend_register_internal_class(&ce);
    kislay_request_ce->create_object = kislay_request_create_object;
    std::memcpy(&kislay_request_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_request_handlers.offset = XtOffsetOf(php_kislay_request_t, std);
    kislay_request_handlers.free_obj = kislay_request_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "Response", kislay_response_methods);
    kislay_response_ce = zend_register_internal_class(&ce);
    kislay_response_ce->create_object = kislay_response_create_object;
    std::memcpy(&kislay_response_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_response_handlers.offset = XtOffsetOf(php_kislay_response_t, std);
    kislay_response_handlers.free_obj = kislay_response_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "App", kislay_app_methods);
    kislay_app_ce = zend_register_internal_class(&ce);
    kislay_app_ce->create_object = kislay_app_create_object;
    std::memcpy(&kislay_app_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_app_handlers.offset = XtOffsetOf(php_kislay_app_t, std);
    kislay_app_handlers.free_obj = kislay_app_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "AsyncHttp", kislay_async_http_methods);
    kislay_async_http_ce = zend_register_internal_class(&ce);
    kislay_async_http_ce->create_object = kislay_async_http_create_object;
    std::memcpy(&kislay_async_http_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_async_http_handlers.offset = XtOffsetOf(php_kislay_async_http_t, std);
    kislay_async_http_handlers.free_obj = kislay_async_http_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "Promise", kislay_promise_methods);
    kislay_promise_ce = zend_register_internal_class(&ce);
    kislay_promise_ce->create_object = kislay_promise_create_object;
    std::memcpy(&kislay_promise_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_promise_handlers.offset = XtOffsetOf(php_kislay_promise_t, std);
    kislay_promise_handlers.free_obj = kislay_promise_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "Kislay", "ServiceClient", kislay_service_client_methods);
    kislay_service_client_ce = zend_register_internal_class(&ce);
    kislay_service_client_ce->create_object = kislay_service_client_create_object;
    std::memcpy(&kislay_service_client_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_service_client_handlers.offset = XtOffsetOf(php_kislay_service_client_t, std);
    kislay_service_client_handlers.free_obj = kislay_service_client_free_obj;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(kislayphp_extension) {
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(kislayphp_extension) {
    php_info_print_table_start();
    php_info_print_table_header(2, "kislayphp_extension support", "enabled");
    php_info_print_table_row(2, "Version", PHP_KISLAYPHP_EXTENSION_VERSION);
    php_info_print_table_end();
}

static PHP_GINIT_FUNCTION(kislayphp_extension) {
    kislayphp_extension_globals->http_threads = 1;
    kislayphp_extension_globals->read_timeout_ms = 10000;
    kislayphp_extension_globals->max_body = 0;
    kislayphp_extension_globals->cors_enabled = 0;
    kislayphp_extension_globals->log_enabled = 1;
    kislayphp_extension_globals->async_enabled = 1;
    kislayphp_extension_globals->gc_enabled = 1;
    kislayphp_extension_globals->document_root = nullptr;
    kislayphp_extension_globals->tls_cert = nullptr;
    kislayphp_extension_globals->tls_key = nullptr;
    kislayphp_extension_globals->referrer_policy = nullptr;
}

zend_module_entry kislayphp_extension_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_KISLAYPHP_EXTENSION_EXTNAME,
    kislay_functions,
    PHP_MINIT(kislayphp_extension),
    PHP_MSHUTDOWN(kislayphp_extension),
    nullptr,
    nullptr,
    PHP_MINFO(kislayphp_extension),
    PHP_KISLAYPHP_EXTENSION_VERSION,
    PHP_MODULE_GLOBALS(kislayphp_extension),
    PHP_GINIT(kislayphp_extension),
    nullptr,
    nullptr,
    STANDARD_MODULE_PROPERTIES_EX
};

#if defined(COMPILE_DL_KISLAYPHP_EXTENSION) || defined(ZEND_COMPILE_DL_EXT)
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
extern "C" {
ZEND_GET_MODULE(kislayphp_extension)
}
#endif
