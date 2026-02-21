extern "C" {
#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "ext/json/php_json.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_gc.h"
#include "Zend/zend_smart_str.h"
}

#include <curl/curl.h>
#include <thread>
#include <future>

#include "php_kislay_extension.h"

#include <civetweb.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <csignal>
#include <mutex>
#include <new>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

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

static zend_class_entry *kislay_app_ce;
static zend_class_entry *kislay_request_ce;
static zend_class_entry *kislay_response_ce;
static zend_class_entry *kislay_next_ce;
static zend_class_entry *kislay_async_http_ce;

ZEND_BEGIN_MODULE_GLOBALS(kislayphp_extension)
    zend_long http_threads;
    zend_long read_timeout_ms;
    zend_long max_body;
    zend_bool cors_enabled;
    zend_bool log_enabled;
    zend_bool async_enabled;
    char *document_root;
    char *tls_cert;
    char *tls_key;
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
    STD_PHP_INI_ENTRY("kislayphp.http.log", "0", PHP_INI_ALL, OnUpdateBool, log_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.async", "0", PHP_INI_ALL, OnUpdateBool, async_enabled, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.document_root", "", PHP_INI_ALL, OnUpdateString, document_root, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.tls_cert", "", PHP_INI_ALL, OnUpdateString, tls_cert, zend_kislayphp_extension_globals, kislayphp_extension_globals)
    STD_PHP_INI_ENTRY("kislayphp.http.tls_key", "", PHP_INI_ALL, OnUpdateString, tls_key, zend_kislayphp_extension_globals, kislayphp_extension_globals)
PHP_INI_END()

namespace kislay {

struct Route {
    std::string method;
    std::string pattern;
    std::regex regex;
    std::vector<std::string> param_names;
    std::vector<zval> middleware;
    zval handler;
};

struct GroupContext {
    std::string prefix;
    std::vector<zval> middleware;
};

struct PathMiddleware {
    std::string prefix;
    zval middleware;
};

}

typedef struct _php_kislay_request_t {
    std::string method;
    std::string uri;
    std::string path;
    std::string query;
    std::string body;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> query_params;
    std::unordered_map<std::string, std::string> body_params;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, zval> attributes;
    zval json_cache;
    bool json_cached;
    bool json_valid;
    zend_object std;
} php_kislay_request_t;

typedef struct _php_kislay_response_t {
    std::string body;
    std::string file_path;
    std::string content_type;
    std::unordered_map<std::string, std::string> headers;
    bool send_file;
    zend_long status_code;
    zend_object std;
} php_kislay_response_t;

typedef struct _php_kislay_next_t {
    bool *continue_flag;
    zend_object std;
} php_kislay_next_t;

typedef struct _php_kislay_app_t {
    std::vector<kislay::Route> routes;
    std::vector<zval> middleware;
    std::vector<kislay::PathMiddleware> path_middleware;
    std::vector<kislay::GroupContext> group_stack;
    std::unordered_map<std::string, size_t> exact_routes;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> exact_routes_by_method;
    std::unordered_map<std::string, std::vector<size_t>> method_routes;
    std::mutex lock;
    struct mg_context *ctx;
    std::atomic_bool running;
    size_t memory_limit_bytes;
    bool gc_after_request;
    int thread_count;
    zend_long read_timeout_ms;
    size_t max_body_bytes;
    bool cors_enabled;
    bool log_enabled;
    bool async_enabled;
    std::string document_root;
    std::string default_tls_cert;
    std::string default_tls_key;
    zend_object std;
} php_kislay_app_t;

static std::atomic<php_kislay_app_t *> kislay_active_app{nullptr};
static std::atomic_bool kislay_signal_stop_requested{false};
static std::atomic_bool kislay_signal_handlers_installed{false};

static int kislay_begin_request(struct mg_connection *conn);
static void kislay_install_signal_handlers();

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
    app->running.store(false, std::memory_order_relaxed);
    kislay_app_clear_active(app);
    kislay_signal_stop_requested.store(false, std::memory_order_relaxed);
}

static bool kislay_app_wait_loop(php_kislay_app_t *app, zend_long timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (app->running.load(std::memory_order_relaxed)) {
        if (kislay_signal_stop_requested.load(std::memory_order_relaxed)) {
            kislay_app_stop_server(app);
            break;
        }
        if (timeout_ms >= 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
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
#if !defined(ZTS)
    if (value > 1) {
        php_error_docref(nullptr, E_WARNING, "%s: Thread Safety is disabled; forcing num_threads=1", source);
        value = 1;
    }
#endif
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
    if (app->max_body_bytes > 0) {
        option_values.push_back("max_request_size");
        option_values.push_back(std::to_string(app->max_body_bytes));
    }
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
    std::string request_body;
    bool use_request_body;
    std::string response_body;
    long response_code;
    zend_object std;
} php_kislay_async_http_t;

static zend_object_handlers kislay_request_handlers;
static zend_object_handlers kislay_response_handlers;
static zend_object_handlers kislay_next_handlers;
static zend_object_handlers kislay_app_handlers;
static zend_object_handlers kislay_async_http_handlers;
static int kislay_begin_request(struct mg_connection *conn);

static inline php_kislay_request_t *php_kislay_request_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_request_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_request_t, std));
}

static inline php_kislay_response_t *php_kislay_response_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_response_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_response_t, std));
}

static inline php_kislay_next_t *php_kislay_next_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_next_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_next_t, std));
}

static inline php_kislay_app_t *php_kislay_app_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_app_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_app_t, std));
}

static inline php_kislay_async_http_t *php_kislay_async_http_from_obj(zend_object *obj) {
    return reinterpret_cast<php_kislay_async_http_t *>(
        reinterpret_cast<char *>(obj) - XtOffsetOf(php_kislay_async_http_t, std));
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
    new (&req->params) std::unordered_map<std::string, std::string>();
    new (&req->query_params) std::unordered_map<std::string, std::string>();
    new (&req->body_params) std::unordered_map<std::string, std::string>();
    new (&req->headers) std::unordered_map<std::string, std::string>();
    new (&req->attributes) std::unordered_map<std::string, zval>();
    ZVAL_UNDEF(&req->json_cache);
    req->json_cached = false;
    req->json_valid = false;
    req->std.handlers = &kislay_request_handlers;
    return &req->std;
}

static void kislay_request_free_obj(zend_object *object) {
    php_kislay_request_t *req = php_kislay_request_from_obj(object);
    for (auto &item : req->attributes) {
        zval_ptr_dtor(&item.second);
    }
    if (req->json_cached && !Z_ISUNDEF(req->json_cache)) {
        zval_ptr_dtor(&req->json_cache);
    }
    req->attributes.~unordered_map();
    req->headers.~unordered_map();
    req->body_params.~unordered_map();
    req->query_params.~unordered_map();
    req->params.~unordered_map();
    req->body.~basic_string();
    req->query.~basic_string();
    req->path.~basic_string();
    req->uri.~basic_string();
    req->method.~basic_string();
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
    res->send_file = false;
    res->status_code = 200;
    res->std.handlers = &kislay_response_handlers;
    return &res->std;
}

static void kislay_response_free_obj(zend_object *object) {
    php_kislay_response_t *res = php_kislay_response_from_obj(object);
    res->headers.~unordered_map();
    res->content_type.~basic_string();
    res->file_path.~basic_string();
    res->body.~basic_string();
    zend_object_std_dtor(&res->std);
}

static zend_object *kislay_next_create_object(zend_class_entry *ce) {
    php_kislay_next_t *next = static_cast<php_kislay_next_t *>(
        ecalloc(1, sizeof(php_kislay_next_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&next->std, ce);
    object_properties_init(&next->std, ce);
    next->continue_flag = nullptr;
    next->std.handlers = &kislay_next_handlers;
    return &next->std;
}

static void kislay_next_free_obj(zend_object *object) {
    php_kislay_next_t *next = php_kislay_next_from_obj(object);
    zend_object_std_dtor(&next->std);
}

static zend_object *kislay_app_create_object(zend_class_entry *ce) {
    php_kislay_app_t *app = static_cast<php_kislay_app_t *>(
        ecalloc(1, sizeof(php_kislay_app_t) + zend_object_properties_size(ce)));
    zend_object_std_init(&app->std, ce);
    object_properties_init(&app->std, ce);
    new (&app->routes) std::vector<kislay::Route>();
    new (&app->middleware) std::vector<zval>();
    new (&app->path_middleware) std::vector<kislay::PathMiddleware>();
    new (&app->group_stack) std::vector<kislay::GroupContext>();
    new (&app->exact_routes) std::unordered_map<std::string, size_t>();
    new (&app->exact_routes_by_method) std::unordered_map<std::string, std::unordered_map<std::string, size_t>>();
    new (&app->method_routes) std::unordered_map<std::string, std::vector<size_t>>();
    new (&app->lock) std::mutex();
    app->ctx = nullptr;
    app->running.store(false, std::memory_order_relaxed);
    app->memory_limit_bytes = 0;
    app->gc_after_request = true;
    app->thread_count = kislay_sanitize_thread_count(
        kislay_env_long("KISLAYPHP_HTTP_THREADS", KISLAYPHP_EXTENSION_G(http_threads)),
        KISLAYPHP_EXTENSION_G(http_threads),
        "Kislay\\Core\\App::__construct"
    );
    app->read_timeout_ms = kislay_sanitize_timeout_ms(
        kislay_env_long("KISLAYPHP_HTTP_READ_TIMEOUT_MS", KISLAYPHP_EXTENSION_G(read_timeout_ms)),
        KISLAYPHP_EXTENSION_G(read_timeout_ms),
        "Kislay\\Core\\App::__construct"
    );
    zend_long max_body = kislay_env_long("KISLAYPHP_HTTP_MAX_BODY", KISLAYPHP_EXTENSION_G(max_body));
    app->max_body_bytes = kislay_sanitize_max_body(
        max_body,
        KISLAYPHP_EXTENSION_G(max_body),
        "Kislay\\Core\\App::__construct"
    );
    app->cors_enabled = kislay_env_bool("KISLAYPHP_HTTP_CORS", KISLAYPHP_EXTENSION_G(cors_enabled) != 0);
    app->log_enabled = kislay_env_bool("KISLAYPHP_HTTP_LOG", KISLAYPHP_EXTENSION_G(log_enabled) != 0);
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
    }
    for (auto &mw : app->middleware) {
        zval_ptr_dtor(&mw);
    }
    for (auto &pmw : app->path_middleware) {
        zval_ptr_dtor(&pmw.middleware);
    }
    for (auto &ctx : app->group_stack) {
        for (auto &mw : ctx.middleware) {
            zval_ptr_dtor(&mw);
        }
    }
    kislay_app_stop_server(app);
    app->group_stack.~vector();
    app->path_middleware.~vector();
    app->middleware.~vector();
    app->routes.~vector();
    app->exact_routes.~unordered_map();
    app->exact_routes_by_method.~unordered_map();
    app->method_routes.~unordered_map();
    app->document_root.~basic_string();
    app->default_tls_cert.~basic_string();
    app->default_tls_key.~basic_string();
    app->lock.~mutex();
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
    new (&async_http->request_body) std::string();
    async_http->use_request_body = false;
    new (&async_http->response_body) std::string();
    async_http->response_code = 0;
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
    async_http->request_body.~basic_string();
    async_http->response_body.~basic_string();
    zend_object_std_dtor(&async_http->std);
}

static std::string kislay_to_upper(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

static std::string kislay_to_lower(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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

static std::string kislay_url_decode(const std::string &value) {
    if (value.empty()) {
        return "";
    }
    std::string out = value;
    size_t new_len = php_url_decode(&out[0], out.size());
    out.resize(new_len);
    return out;
}

static void kislay_parse_query(const char *query, std::unordered_map<std::string, std::string> &out) {
    if (query == nullptr || *query == '\0') {
        return;
    }
    const char *start = query;
    const char *cur = query;
    while (true) {
        if (*cur == '&' || *cur == '\0') {
            std::string pair(start, static_cast<size_t>(cur - start));
            if (!pair.empty()) {
                size_t eq = pair.find('=');
                std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
                std::string val = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
                key = kislay_url_decode(key);
                val = kislay_url_decode(val);
                if (!key.empty()) {
                    out[key] = val;
                }
            }
            if (*cur == '\0') {
                break;
            }
            start = cur + 1;
        }
        ++cur;
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
    std::fprintf(stderr,
                 "[kislay] time=\"%s\" request=\"%s %s\" response=\"%lld %lldB\" duration_ms=%lld error=\"%s\"\n",
                 kislay_now_timestamp().c_str(),
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

static bool kislay_build_route(const std::string &pattern, kislay::Route *route) {
    std::string regex_str;
    regex_str.reserve(pattern.size() * 2);
    regex_str += "^";

    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == ':' && i > 0 && pattern[i - 1] == '/') {
            std::string name;
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != '/') {
                name.push_back(pattern[j]);
                ++j;
            }
            if (name.empty()) {
                return false;
            }
            route->param_names.push_back(name);
            regex_str += "([^/]+)";
            i = j - 1;
            continue;
        }

        if (std::strchr(".^$|()[]*+?{}\\", ch) != nullptr) {
            regex_str.push_back('\\');
        }
        regex_str.push_back(ch);
    }

    regex_str += "$";
    try {
        route->regex = std::regex(regex_str, std::regex::ECMAScript);
    } catch (const std::regex_error &) {
        return false;
    }
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

static void kislay_request_fill_params(php_kislay_request_t *req, const kislay::Route &route, const std::smatch &match) {
    req->params.clear();
    for (size_t i = 0; i < route.param_names.size(); ++i) {
        if (i + 1 < match.size()) {
            req->params[route.param_names[i]] = match[i + 1].str();
        }
    }
}

static void kislay_response_set_body(php_kislay_response_t *res, const char *data, size_t len) {
    res->send_file = false;
    res->file_path.clear();
    res->headers.erase("content-length");
    res->body.assign(data, len);
}

static void kislay_send_response(struct mg_connection *conn, php_kislay_response_t *res, bool cors_enabled) {
    zend_long status_code = res->status_code;
    if (!kislay_is_valid_http_status(status_code)) {
        status_code = 500;
    }
    const char *status_text = kislay_status_text(status_code);
    const std::string &body = res->body;
    const bool stream_file = res->send_file && !res->file_path.empty();
    std::string content_type = res->content_type.empty() ? "text/plain" : res->content_type;
    std::string content_length = std::to_string(body.size());
    auto header_cl = res->headers.find("content-length");
    auto header_ct = res->headers.find("content-type");
    if (header_ct != res->headers.end()) {
        content_type = header_ct->second;
    }
    if (stream_file && (header_cl == res->headers.end() || header_cl->second.empty())) {
        struct stat file_stat;
        if (stat(res->file_path.c_str(), &file_stat) == 0 && file_stat.st_size >= 0) {
            content_length = std::to_string(static_cast<size_t>(file_stat.st_size));
        }
    }
    if (header_cl != res->headers.end() && !header_cl->second.empty()) {
        content_length = header_cl->second;
    }
    mg_printf(conn,
              "HTTP/1.1 %lld %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %s\r\n"
              "Connection: close\r\n",
              static_cast<long long>(status_code),
              status_text,
              content_type.c_str(),
              content_length.c_str());

    if (cors_enabled) {
        mg_printf(conn,
                  "Access-Control-Allow-Origin: *\r\n"
                  "Access-Control-Allow-Private-Network: true\r\n"
                  "Access-Control-Allow-Headers: *\r\n"
                  "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n");
    }

    for (const auto &header : res->headers) {
        if (header.first == "content-type" || header.first == "content-length") {
            continue;
        }
        mg_printf(conn, "%s: %s\r\n", header.first.c_str(), header.second.c_str());
    }

    mg_printf(conn, "\r\n");
    if (stream_file) {
        mg_send_file_body(conn, res->file_path.c_str());
    } else if (!body.empty()) {
        mg_write(conn, body.c_str(), body.size());
    }
}

static bool kislay_call_php(zval *callable, uint32_t argc, zval *argv, zval *retval) {
    ZVAL_UNDEF(retval);
    if (call_user_function(EG(function_table), nullptr, callable, retval, argc, argv) == FAILURE) {
        return false;
    }
    return true;
}

static bool kislay_run_middleware_list(const std::vector<zval> &list, zval *req_obj, zval *res_obj) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(res_obj));
    for (const auto &mw : list) {
        bool next_called = false;
        zval next_obj;
        object_init_ex(&next_obj, kislay_next_ce);
        php_kislay_next_t *next = php_kislay_next_from_obj(Z_OBJ(next_obj));
        next->continue_flag = &next_called;

        zval args[3];
        ZVAL_COPY(&args[0], req_obj);
        ZVAL_COPY(&args[1], res_obj);
        ZVAL_COPY(&args[2], &next_obj);

        zval retval;
        bool ok = kislay_call_php(const_cast<zval *>(&mw), 3, args, &retval);

        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&args[1]);
        zval_ptr_dtor(&args[2]);
        if (ok) {
            zval_ptr_dtor(&retval);
        }

        if (!ok || EG(exception)) {
            kislay_mark_internal_error(res);
            return false;
        }
        if (!next_called) {
            if (!kislay_response_has_content(res)) {
                res->status_code = 500;
                res->body = "Middleware chain stopped without response";
            }
            return false;
        }
    }
    return true;
}

static bool kislay_run_middleware(php_kislay_app_t *app, zval *req_obj, zval *res_obj) {
    return kislay_run_middleware_list(app->middleware, req_obj, res_obj);
}

static bool kislay_handle_route(php_kislay_app_t *app, const std::string &method, const std::string &uri, zval *req_obj, zval *res_obj) {
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(res_obj));
    for (auto &route : app->routes) {
        if (route.method != method) {
            continue;
        }
        std::smatch match;
        if (!std::regex_match(uri, match, route.regex)) {
            continue;
        }
        php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(req_obj));
        kislay_request_fill_params(req, route, match);
        if (!route.middleware.empty()) {
            if (!kislay_run_middleware_list(route.middleware, req_obj, res_obj)) {
                return false;
            }
        }

        zval args[2];
        ZVAL_COPY(&args[0], req_obj);
        ZVAL_COPY(&args[1], res_obj);
        zval retval;
        bool ok = kislay_call_php(&route.handler, 2, args, &retval);
        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&args[1]);
        if (ok) {
            zval_ptr_dtor(&retval);
        }
        if (!ok || EG(exception)) {
            kislay_mark_internal_error(res);
            return false;
        }
        return true;
    }
    return false;
}

static int kislay_begin_request(struct mg_connection *conn) {
    const struct mg_request_info *info = mg_get_request_info(conn);
    if (info == nullptr || info->user_data == nullptr) {
        return 0;
    }

    const auto request_start = std::chrono::steady_clock::now();
    auto *app = static_cast<php_kislay_app_t *>(info->user_data);

    std::string method = info->request_method ? info->request_method : "";
    std::string log_method = kislay_to_upper(method);
    std::string uri = info->local_uri ? info->local_uri : (info->request_uri ? info->request_uri : "");
    size_t query_pos = uri.find('?');
    if (query_pos != std::string::npos) {
        uri = uri.substr(0, query_pos);
    }
    std::string route_uri = kislay_normalize_route_path(uri);
    std::string query = info->query_string ? info->query_string : "";

    // Fast path for simple routes - DISABLED to ensure all requests go through PHP
    // if (method == "GET" && uri == "/hello") {
    //     mg_printf(conn, "HTTP/1.1 200 OK\r\n"
    //                     "Content-Type: text/plain\r\n"
    //                     "Content-Length: 11\r\n"
    //                     "Connection: close\r\n"
    //                     "\r\n"
    //                     "Hello World");
    //     return 1;
    // }

    if (app->cors_enabled && method == "OPTIONS") {
        mg_printf(conn, "HTTP/1.1 200 OK\r\n"
                        "Allow: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Private-Network: true\r\n"
                        "Access-Control-Allow-Headers: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n");
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_start
        ).count();
        kislay_log_request_record(app, log_method, uri, 200, 0, duration_ms, "");
        return 1;
    }

    if (app->max_body_bytes > 0 && info->content_length > static_cast<long long>(app->max_body_bytes)) {
        zval res_obj;
        object_init_ex(&res_obj, kislay_response_ce);
        php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ(res_obj));
        res->status_code = 413;
        res->body = "Payload Too Large";
        kislay_send_response(conn, res, app->cors_enabled);
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_start
        ).count();
        kislay_log_request_record(
            app,
            log_method,
            uri,
            res->status_code,
            kislay_response_size_bytes(res),
            duration_ms,
            res->body
        );
        zval_ptr_dtor(&res_obj);
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

    zval req_obj;
    object_init_ex(&req_obj, kislay_request_ce);
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ(req_obj));
    req->method = kislay_to_upper(method);
    req->uri = uri;
    req->path = route_uri;
    req->query = query;
    req->body = body;
    req->headers.clear();
    for (int i = 0; i < info->num_headers; ++i) {
        if (info->http_headers[i].name && info->http_headers[i].value) {
            std::string name = kislay_to_lower(info->http_headers[i].name);
            req->headers[name] = info->http_headers[i].value;
        }
    }
    req->query_params.clear();
    kislay_parse_query(info->query_string, req->query_params);
    req->body_params.clear();
    auto ct_it = req->headers.find("content-type");
    if (ct_it != req->headers.end()) {
        std::string ct = kislay_to_lower(ct_it->second);
        if (ct.find("application/x-www-form-urlencoded") != std::string::npos) {
            kislay_parse_query(req->body.c_str(), req->body_params);
        }
    }

    zval res_obj;
    object_init_ex(&res_obj, kislay_response_ce);
    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ(res_obj));

    bool handled = false;
    bool run_routes = true;
    const std::vector<zval> *app_middleware = &app->middleware;
    const std::vector<zval> *route_middleware = nullptr;
    const zval *route_handler = nullptr;
    bool route_matched = false;
    std::string request_error;

    if (app->memory_limit_bytes > 0 && zend_memory_usage(0) > app->memory_limit_bytes) {
        res->status_code = 500;
        res->body = "Memory limit exceeded";
        kislay_send_response(conn, res, app->cors_enabled);
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_start
        ).count();
        kislay_log_request_record(
            app,
            req->method,
            uri,
            res->status_code,
            kislay_response_size_bytes(res),
            duration_ms,
            res->body
        );
        zval_ptr_dtor(&req_obj);
        zval_ptr_dtor(&res_obj);
        return 1;
    }

    auto method_exact_it = app->exact_routes_by_method.find(req->method);
    if (method_exact_it != app->exact_routes_by_method.end()) {
        auto uri_exact_it = method_exact_it->second.find(route_uri);
        if (uri_exact_it != method_exact_it->second.end() && uri_exact_it->second < app->routes.size()) {
            const auto &route = app->routes[uri_exact_it->second];
            req->params.clear();
            route_middleware = &route.middleware;
            route_handler = &route.handler;
            route_matched = true;
        }
    }
    if (!route_matched) {
        auto any_exact_it = app->exact_routes_by_method.find("*");
        if (any_exact_it != app->exact_routes_by_method.end()) {
            auto uri_exact_it = any_exact_it->second.find(route_uri);
            if (uri_exact_it != any_exact_it->second.end() && uri_exact_it->second < app->routes.size()) {
                const auto &route = app->routes[uri_exact_it->second];
                req->params.clear();
                route_middleware = &route.middleware;
                route_handler = &route.handler;
                route_matched = true;
            }
        }
    }

    if (!route_matched) {
        auto method_routes_it = app->method_routes.find(req->method);
        if (method_routes_it != app->method_routes.end()) {
            for (size_t route_index : method_routes_it->second) {
                if (route_index >= app->routes.size()) {
                    continue;
                }
                auto &route = app->routes[route_index];
                std::smatch match;
                if (!std::regex_match(route_uri, match, route.regex)) {
                    continue;
                }
                req->params.clear();
                for (size_t i = 0; i < route.param_names.size(); ++i) {
                    if (i + 1 < match.size()) {
                        req->params[route.param_names[i]] = match[i + 1].str();
                    } else {
                        req->params[route.param_names[i]] = "";
                    }
                }
                route_middleware = &route.middleware;
                route_handler = &route.handler;
                route_matched = true;
                break;
            }
        }
    }
    if (!route_matched) {
        auto any_routes_it = app->method_routes.find("*");
        if (any_routes_it != app->method_routes.end()) {
            for (size_t route_index : any_routes_it->second) {
                if (route_index >= app->routes.size()) {
                    continue;
                }
                auto &route = app->routes[route_index];
                std::smatch match;
                if (!std::regex_match(route_uri, match, route.regex)) {
                    continue;
                }
                req->params.clear();
                for (size_t i = 0; i < route.param_names.size(); ++i) {
                    if (i + 1 < match.size()) {
                        req->params[route.param_names[i]] = match[i + 1].str();
                    } else {
                        req->params[route.param_names[i]] = "";
                    }
                }
                route_middleware = &route.middleware;
                route_handler = &route.handler;
                route_matched = true;
                break;
            }
        }
    }

    run_routes = kislay_run_middleware_list(*app_middleware, &req_obj, &res_obj);
    if (run_routes && !app->path_middleware.empty()) {
        std::vector<zval> scoped_middleware;
        scoped_middleware.reserve(app->path_middleware.size());
        for (const auto &pmw : app->path_middleware) {
            if (kislay_path_has_prefix(route_uri, pmw.prefix)) {
                scoped_middleware.push_back(pmw.middleware);
                Z_TRY_ADDREF(scoped_middleware.back());
            }
        }
        if (!scoped_middleware.empty()) {
            run_routes = kislay_run_middleware_list(scoped_middleware, &req_obj, &res_obj);
            for (auto &mw : scoped_middleware) {
                zval_ptr_dtor(&mw);
            }
        }
    }
    if (run_routes && route_handler != nullptr) {
        if (route_middleware != nullptr && !route_middleware->empty()) {
            run_routes = kislay_run_middleware_list(*route_middleware, &req_obj, &res_obj);
        }
        if (run_routes) {
            zval args[2];
            ZVAL_COPY(&args[0], &req_obj);
            ZVAL_COPY(&args[1], &res_obj);
            zval retval;
            bool ok = kislay_call_php(const_cast<zval *>(route_handler), 2, args, &retval);
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&args[1]);
            if (ok) {
                zval_ptr_dtor(&retval);
            }
            if (!ok || EG(exception)) {
                kislay_mark_internal_error(res);
                run_routes = false;
            } else {
                handled = true;
            }
        }
    }

    if (!handled && !kislay_response_has_content(res)) {
        if (route_matched) {
            res->status_code = 500;
            res->body = "Internal Server Error";
            request_error = res->body;
        } else {
            res->status_code = 404;
            res->body = "Not Found";
            request_error = res->body;
        }
    }

    kislay_send_response(conn, res, app->cors_enabled);
    if (request_error.empty() && res->status_code >= 400) {
        request_error = res->body;
    }
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - request_start
    ).count();
    kislay_log_request_record(
        app,
        req->method,
        uri,
        res->status_code,
        kislay_response_size_bytes(res),
        duration_ms,
        request_error
    );

    zval_ptr_dtor(&req_obj);
    zval_ptr_dtor(&res_obj);

    if (app->gc_after_request) {
        gc_collect_cycles();
    }
    return 1;
}

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

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_enable_async, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_set_option, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
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
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
    }
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
    std::string key(name, name_len);
    auto it = req->query_params.find(key);
    if (it == req->query_params.end()) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_STRING(it->second.c_str());
}

PHP_METHOD(KislayRequest, getQueryParams) {
    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    array_init(return_value);
    for (const auto &item : req->query_params) {
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
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
    std::string key(name, name_len);
    auto it = req->params.find(key);
    if (it != req->params.end()) {
        RETURN_STRING(it->second.c_str());
    }
    it = req->body_params.find(key);
    if (it != req->body_params.end()) {
        RETURN_STRING(it->second.c_str());
    }
    it = req->query_params.find(key);
    if (it != req->query_params.end()) {
        RETURN_STRING(it->second.c_str());
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
    array_init(return_value);

    for (const auto &item : req->query_params) {
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
    }
    for (const auto &item : req->body_params) {
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
    }
    for (const auto &item : req->params) {
        add_assoc_string(return_value, item.first.c_str(), item.second.c_str());
    }
}

PHP_METHOD(KislayRequest, has) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key(name, name_len);
    if (req->params.find(key) != req->params.end()) {
        RETURN_TRUE;
    }
    if (req->body_params.find(key) != req->body_params.end()) {
        RETURN_TRUE;
    }
    if (req->query_params.find(key) != req->query_params.end()) {
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
    array_init(return_value);

    HashTable *ht = Z_ARRVAL_P(keys);
    zval *entry = nullptr;
    ZEND_HASH_FOREACH_VAL(ht, entry) {
        zend_string *key_str = zval_get_string(entry);
        std::string key(ZSTR_VAL(key_str), ZSTR_LEN(key_str));
        zend_string_release(key_str);

        auto it = req->params.find(key);
        if (it != req->params.end()) {
            add_assoc_string(return_value, key.c_str(), it->second.c_str());
            continue;
        }
        it = req->body_params.find(key);
        if (it != req->body_params.end()) {
            add_assoc_string(return_value, key.c_str(), it->second.c_str());
            continue;
        }
        it = req->query_params.find(key);
        if (it != req->query_params.end()) {
            add_assoc_string(return_value, key.c_str(), it->second.c_str());
            continue;
        }
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
    auto it = req->attributes.find(key);
    if (it != req->attributes.end()) {
        zval_ptr_dtor(&it->second);
        req->attributes.erase(it);
    }
    zval copy;
    ZVAL_COPY(&copy, value);
    req->attributes[key] = copy;
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
    auto it = req->attributes.find(key);
    if (it == req->attributes.end()) {
        if (default_val != nullptr) {
            RETURN_ZVAL(default_val, 1, 0);
        }
        RETURN_NULL();
    }
    RETURN_ZVAL(&it->second, 1, 0);
}

PHP_METHOD(KislayRequest, hasAttribute) {
    char *name = nullptr;
    size_t name_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_request_t *req = php_kislay_request_from_obj(Z_OBJ_P(getThis()));
    std::string key(name, name_len);
    RETURN_BOOL(req->attributes.find(key) != req->attributes.end());
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

    zend_string *body_str = zval_get_string(body);
    kislay_response_set_body(res, ZSTR_VAL(body_str), ZSTR_LEN(body_str));
    zend_string_release(body_str);
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
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    if (buf.s != nullptr) {
        kislay_response_set_body(res, ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
    } else {
        kislay_response_set_body(res, "", 0);
    }
    smart_str_free(&buf);

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
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    if (buf.s != nullptr) {
        kislay_response_set_body(res, ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
    } else {
        kislay_response_set_body(res, "", 0);
    }
    smart_str_free(&buf);

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

PHP_METHOD(KislayNext, __invoke) {
    php_kislay_next_t *next = php_kislay_next_from_obj(Z_OBJ_P(getThis()));
    if (next->continue_flag != nullptr) {
        *next->continue_flag = true;
    }
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
    if (key == "log" || key == "log_enabled") {
        app->log_enabled = kislay_zval_to_bool(value, app->log_enabled, "Kislay\\Core\\App::setOption(log)");
        RETURN_TRUE;
    }
    if (key == "async" || key == "async_enabled") {
        app->async_enabled = kislay_zval_to_bool(value, app->async_enabled, "Kislay\\Core\\App::setOption(async)");
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

    php_error_docref(nullptr, E_WARNING, "Unsupported option \"%s\"; ignoring and keeping defaults", name);
    RETURN_TRUE;
}

static bool kislay_app_add_route(php_kislay_app_t *app, const std::string &method, const std::string &pattern, zval *handler) {
    if (!kislay_is_callable(handler)) {
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
    if (!kislay_build_route(route.pattern, &route)) {
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
    const bool is_exact = route.param_names.empty();
    const std::string exact_key = route.method + " " + route.pattern;
    size_t index = app->routes.size();
    app->routes.push_back(std::move(route));
    app->method_routes[app->routes[index].method].push_back(index);
    if (is_exact) {
        app->exact_routes[exact_key] = index;
        app->exact_routes_by_method[app->routes[index].method][app->routes[index].pattern] = index;
    }
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
        std::lock_guard<std::mutex> guard(app->lock);
        zval copy;
        ZVAL_COPY(&copy, path_or_callable);
        app->middleware.push_back(copy);
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

    std::lock_guard<std::mutex> guard(app->lock);
    kislay::PathMiddleware scoped;
    scoped.prefix = prefix;
    ZVAL_COPY(&scoped.middleware, callable);
    app->path_middleware.push_back(std::move(scoped));
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "GET", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "POST", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "PUT", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "PATCH", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "DELETE", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "OPTIONS", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "ALL", std::string(path, path_len), handler)) {
        zend_throw_exception(zend_ce_exception, "Invalid route or handler", 0);
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
        std::lock_guard<std::mutex> guard(app->lock);
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
        std::lock_guard<std::mutex> guard(app->lock);
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
    kislay_sanitize_tls_paths(&cert_path, &key_path, "Kislay\\Core\\App::listen");
    app->thread_count = kislay_sanitize_thread_count(app->thread_count, KISLAYPHP_EXTENSION_G(http_threads), "Kislay\\Core\\App::listen");
    app->read_timeout_ms = kislay_sanitize_timeout_ms(app->read_timeout_ms, KISLAYPHP_EXTENSION_G(read_timeout_ms), "Kislay\\Core\\App::listen");

    if (!kislay_app_start_server(app, listen_addr, cert_path, key_path)) {
        zend_throw_exception(zend_ce_exception, "Failed to start server", 0);
        RETURN_FALSE;
    }

    kislay_app_wait_loop(app, -1);

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
    app->document_root = kislay_sanitize_document_root(app->document_root, "", "Kislay\\Core\\App::listenAsync");
    kislay_sanitize_tls_paths(&cert_path, &key_path, "Kislay\\Core\\App::listenAsync");
    app->thread_count = kislay_sanitize_thread_count(app->thread_count, KISLAYPHP_EXTENSION_G(http_threads), "Kislay\\Core\\App::listenAsync");
    app->read_timeout_ms = kislay_sanitize_timeout_ms(app->read_timeout_ms, KISLAYPHP_EXTENSION_G(read_timeout_ms), "Kislay\\Core\\App::listenAsync");

    if (!kislay_app_start_server(app, listen_addr, cert_path, key_path)) {
        zend_throw_exception(zend_ce_exception, "Failed to start server", 0);
        RETURN_FALSE;
    }

    RETURN_TRUE;
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
    async_http->request_body.clear();
    async_http->use_request_body = false;

    std::string target_url(ZSTR_VAL(url), ZSTR_LEN(url));
    if (data != nullptr) {
        std::string query = kislay_async_http_build_query(async_http->curl, Z_ARRVAL_P(data));
        target_url = kislay_async_http_append_query(url, query);
    }

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
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    php_kislay_async_http_t *async_http = (php_kislay_async_http_t *)userp;
    async_http->response_body.append((char*)contents, realsize);
    return realsize;
}

PHP_METHOD(KislayAsyncHttp, execute) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    
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

PHP_METHOD(KislayAsyncHttp, getResponse) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    RETURN_STRING(async_http->response_body.c_str());
}

PHP_METHOD(KislayAsyncHttp, getResponseCode) {
    php_kislay_async_http_t *async_http = php_kislay_async_http_from_obj(Z_OBJ_P(getThis()));
    RETURN_LONG(async_http->response_code);
}

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

static const zend_function_entry kislay_next_methods[] = {
    PHP_ME(KislayNext, __invoke, arginfo_kislay_void, ZEND_ACC_PUBLIC)
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
    PHP_ME(KislayAsyncHttp, execute, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, getResponse, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayAsyncHttp, getResponseCode, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(kislayphp_extension) {
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

    INIT_NS_CLASS_ENTRY(ce, "Kislay\\Core", "Next", kislay_next_methods);
    kislay_next_ce = zend_register_internal_class(&ce);
    kislay_next_ce->create_object = kislay_next_create_object;
    std::memcpy(&kislay_next_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_next_handlers.offset = XtOffsetOf(php_kislay_next_t, std);
    kislay_next_handlers.free_obj = kislay_next_free_obj;

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
    kislayphp_extension_globals->log_enabled = 0;
    kislayphp_extension_globals->async_enabled = 0;
    kislayphp_extension_globals->document_root = nullptr;
    kislayphp_extension_globals->tls_cert = nullptr;
    kislayphp_extension_globals->tls_key = nullptr;
}

zend_module_entry kislayphp_extension_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_KISLAYPHP_EXTENSION_EXTNAME,
    nullptr,
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
