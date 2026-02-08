extern "C" {
#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "ext/json/php_json.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_gc.h"
#include "Zend/zend_smart_str.h"
}

#include "php_kislay_extension.h"

#include <civetweb.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <new>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static zend_long kislay_env_long(const char *name, zend_long fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return static_cast<zend_long>(std::strtoll(value, nullptr, 10));
}

static bool kislay_env_bool(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0) {
        return false;
    }
    return fallback;
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

ZEND_BEGIN_MODULE_GLOBALS(kislayphp_extension)
    zend_long http_threads;
    zend_long read_timeout_ms;
    zend_long max_body;
    zend_bool cors_enabled;
    zend_bool log_enabled;
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

}

typedef struct _php_kislay_request_t {
    zend_object std;
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
} php_kislay_request_t;

typedef struct _php_kislay_response_t {
    zend_object std;
    std::string body;
    std::string content_type;
    std::unordered_map<std::string, std::string> headers;
    zend_long status_code;
} php_kislay_response_t;

typedef struct _php_kislay_next_t {
    zend_object std;
    bool *continue_flag;
} php_kislay_next_t;

typedef struct _php_kislay_app_t {
    zend_object std;
    std::vector<kislay::Route> routes;
    std::vector<zval> middleware;
    std::vector<kislay::GroupContext> group_stack;
    std::mutex lock;
    struct mg_context *ctx;
    bool running;
    size_t memory_limit_bytes;
    bool gc_after_request;
    int thread_count;
    zend_long read_timeout_ms;
    size_t max_body_bytes;
    bool cors_enabled;
    bool log_enabled;
    std::string default_tls_cert;
    std::string default_tls_key;
} php_kislay_app_t;

static zend_object_handlers kislay_request_handlers;
static zend_object_handlers kislay_response_handlers;
static zend_object_handlers kislay_next_handlers;
static zend_object_handlers kislay_app_handlers;

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
    req->std.handlers = &kislay_request_handlers;
    return &req->std;
}

static void kislay_request_free_obj(zend_object *object) {
    php_kislay_request_t *req = php_kislay_request_from_obj(object);
    for (auto &item : req->attributes) {
        zval_ptr_dtor(&item.second);
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
    new (&res->content_type) std::string();
    new (&res->headers) std::unordered_map<std::string, std::string>();
    res->status_code = 200;
    res->std.handlers = &kislay_response_handlers;
    return &res->std;
}

static void kislay_response_free_obj(zend_object *object) {
    php_kislay_response_t *res = php_kislay_response_from_obj(object);
    res->headers.~unordered_map();
    res->content_type.~basic_string();
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
    new (&app->group_stack) std::vector<kislay::GroupContext>();
    new (&app->lock) std::mutex();
    app->ctx = nullptr;
    app->running = false;
    app->memory_limit_bytes = 0;
    app->gc_after_request = true;
    app->thread_count = static_cast<int>(kislay_env_long("KISLAYPHP_HTTP_THREADS", KISLAYPHP_EXTENSION_G(http_threads)));
    app->read_timeout_ms = kislay_env_long("KISLAYPHP_HTTP_READ_TIMEOUT_MS", KISLAYPHP_EXTENSION_G(read_timeout_ms));
    app->max_body_bytes = static_cast<size_t>(kislay_env_long("KISLAYPHP_HTTP_MAX_BODY", KISLAYPHP_EXTENSION_G(max_body)));
    app->cors_enabled = kislay_env_bool("KISLAYPHP_HTTP_CORS", KISLAYPHP_EXTENSION_G(cors_enabled) != 0);
    app->log_enabled = kislay_env_bool("KISLAYPHP_HTTP_LOG", KISLAYPHP_EXTENSION_G(log_enabled) != 0);
    new (&app->default_tls_cert) std::string(kislay_env_string("KISLAYPHP_HTTP_TLS_CERT", KISLAYPHP_EXTENSION_G(tls_cert) ? KISLAYPHP_EXTENSION_G(tls_cert) : ""));
    new (&app->default_tls_key) std::string(kislay_env_string("KISLAYPHP_HTTP_TLS_KEY", KISLAYPHP_EXTENSION_G(tls_key) ? KISLAYPHP_EXTENSION_G(tls_key) : ""));
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
    for (auto &ctx : app->group_stack) {
        for (auto &mw : ctx.middleware) {
            zval_ptr_dtor(&mw);
        }
    }
    if (app->ctx != nullptr) {
        mg_stop(app->ctx);
        app->ctx = nullptr;
    }
    app->group_stack.~vector();
    app->middleware.~vector();
    app->routes.~vector();
    app->default_tls_cert.~basic_string();
    app->default_tls_key.~basic_string();
    app->lock.~mutex();
    zend_object_std_dtor(&app->std);
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

static std::string kislay_url_decode(const std::string &value) {
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

static void kislay_send_response(struct mg_connection *conn, php_kislay_response_t *res, bool cors_enabled) {
    const char *status_text = "OK";
    if (res->status_code >= 400) {
        status_text = "Error";
    }
    const std::string &body = res->body;
    std::string content_type = res->content_type.empty() ? "text/plain" : res->content_type;
    auto header_ct = res->headers.find("content-type");
    if (header_ct != res->headers.end()) {
        content_type = header_ct->second;
    }
    mg_printf(conn,
              "HTTP/1.1 %lld %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "Connection: close\r\n",
              static_cast<long long>(res->status_code),
              status_text,
              content_type.c_str(),
              body.size());

    if (cors_enabled) {
        mg_printf(conn,
                  "Access-Control-Allow-Origin: *\r\n"
                  "Access-Control-Allow-Private-Network: true\r\n"
                  "Access-Control-Allow-Headers: *\r\n"
                  "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n");
    }

    for (const auto &header : res->headers) {
        if (header.first == "content-type") {
            continue;
        }
        mg_printf(conn, "%s: %s\r\n", header.first.c_str(), header.second.c_str());
    }

    mg_printf(conn, "\r\n");
    if (!body.empty()) {
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

static void kislay_copy_middleware_list(const std::vector<zval> &source, std::vector<zval> &dest) {
    dest.clear();
    dest.reserve(source.size());
    for (const auto &mw : source) {
        zval copy;
        ZVAL_COPY(&copy, const_cast<zval *>(&mw));
        dest.push_back(copy);
    }
}

static void kislay_free_middleware_list(std::vector<zval> &list) {
    for (auto &mw : list) {
        zval_ptr_dtor(&mw);
    }
    list.clear();
}

static bool kislay_run_middleware_list(std::vector<zval> &list, zval *req_obj, zval *res_obj) {
    for (auto &mw : list) {
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
        bool ok = kislay_call_php(&mw, 3, args, &retval);

        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&args[1]);
        zval_ptr_dtor(&args[2]);
        if (ok) {
            zval_ptr_dtor(&retval);
        }

        if (!ok || EG(exception)) {
            if (EG(exception)) {
                zend_clear_exception();
            }
            return false;
        }
        if (!next_called) {
            return false;
        }
    }
    return true;
}

static bool kislay_run_middleware(php_kislay_app_t *app, zval *req_obj, zval *res_obj) {
    return kislay_run_middleware_list(app->middleware, req_obj, res_obj);
}

static bool kislay_handle_route(php_kislay_app_t *app, const std::string &method, const std::string &uri, zval *req_obj, zval *res_obj) {
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
            if (EG(exception)) {
                zend_clear_exception();
            }
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

    auto *app = static_cast<php_kislay_app_t *>(info->user_data);

    std::string method = info->request_method ? info->request_method : "";
    std::string uri = info->local_uri ? info->local_uri : (info->request_uri ? info->request_uri : "");
    std::string query = info->query_string ? info->query_string : "";

    if (app->cors_enabled && method == "OPTIONS") {
        zval res_obj;
        object_init_ex(&res_obj, kislay_response_ce);
        php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ(res_obj));
        res->status_code = 200;
        res->body.clear();
        kislay_send_response(conn, res, app->cors_enabled);
        zval_ptr_dtor(&res_obj);
        return 1;
    }

    if (app->max_body_bytes > 0 && info->content_length > static_cast<long long>(app->max_body_bytes)) {
        zval res_obj;
        object_init_ex(&res_obj, kislay_response_ce);
        php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ(res_obj));
        res->status_code = 413;
        res->body = "Payload Too Large";
        kislay_send_response(conn, res, app->cors_enabled);
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
    req->path = uri;
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
    std::vector<zval> app_middleware;
    std::vector<zval> route_middleware;
    zval route_handler;
    ZVAL_UNDEF(&route_handler);
    std::vector<std::string> route_param_names;
    std::vector<std::string> route_param_values;
    bool route_matched = false;

    {
        std::lock_guard<std::mutex> guard(app->lock);
        if (app->memory_limit_bytes > 0 && zend_memory_usage(0) > app->memory_limit_bytes) {
            res->status_code = 500;
            res->body = "Memory limit exceeded";
            kislay_send_response(conn, res, app->cors_enabled);
            zval_ptr_dtor(&req_obj);
            zval_ptr_dtor(&res_obj);
            return 1;
        }

        kislay_copy_middleware_list(app->middleware, app_middleware);

        for (auto &route : app->routes) {
            if (route.method != req->method) {
                continue;
            }
            std::smatch match;
            if (!std::regex_match(uri, match, route.regex)) {
                continue;
            }
            route_param_names = route.param_names;
            route_param_values.clear();
            for (size_t i = 0; i < route.param_names.size(); ++i) {
                if (i + 1 < match.size()) {
                    route_param_values.push_back(match[i + 1].str());
                } else {
                    route_param_values.push_back("");
                }
            }
            kislay_copy_middleware_list(route.middleware, route_middleware);
            ZVAL_COPY(&route_handler, &route.handler);
            route_matched = true;
            break;
        }
    }

    if (route_matched) {
        req->params.clear();
        for (size_t i = 0; i < route_param_names.size(); ++i) {
            req->params[route_param_names[i]] = route_param_values[i];
        }
    }

    run_routes = kislay_run_middleware_list(app_middleware, &req_obj, &res_obj);
    if (run_routes && !Z_ISUNDEF(route_handler)) {
        if (!route_middleware.empty()) {
            run_routes = kislay_run_middleware_list(route_middleware, &req_obj, &res_obj);
        }
        if (run_routes) {
            zval args[2];
            ZVAL_COPY(&args[0], &req_obj);
            ZVAL_COPY(&args[1], &res_obj);
            zval retval;
            bool ok = kislay_call_php(&route_handler, 2, args, &retval);
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&args[1]);
            if (ok) {
                zval_ptr_dtor(&retval);
            }
            if (!ok || EG(exception)) {
                if (EG(exception)) {
                    zend_clear_exception();
                }
                run_routes = false;
            } else {
                handled = true;
            }
        }
    }

    kislay_free_middleware_list(app_middleware);
    kislay_free_middleware_list(route_middleware);
    if (!Z_ISUNDEF(route_handler)) {
        zval_ptr_dtor(&route_handler);
    }

    if (!handled && res->body.empty()) {
        res->status_code = 404;
        res->body = "Not Found";
    }

    kislay_send_response(conn, res, app->cors_enabled);

    if (app->log_enabled) {
        std::fprintf(stderr, "[kislay] %s %s %lld\n", req->method.c_str(), uri.c_str(), static_cast<long long>(res->status_code));
    }

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
    ZEND_ARG_CALLABLE_INFO(0, middleware, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_kislay_app_route, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_CALLABLE_INFO(0, handler, 0)
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

PHP_METHOD(KislayResponse, setBody) {
    char *body = nullptr;
    size_t body_len = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(body, body_len)
    ZEND_PARSE_PARAMETERS_END();

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->body.assign(body, body_len);
}

PHP_METHOD(KislayResponse, setStatusCode) {
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

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

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    res->status_code = status;
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
        res->status_code = status;
    }

    zend_string *body_str = zval_get_string(body);
    res->body.assign(ZSTR_VAL(body_str), ZSTR_LEN(body_str));
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

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    if (buf.s != nullptr) {
        res->body.assign(ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
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
    res->body.assign(xml, xml_len);
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

    php_kislay_response_t *res = php_kislay_response_from_obj(Z_OBJ_P(getThis()));
    smart_str buf = {0};
    php_json_encode(&buf, data, 0);
    smart_str_0(&buf);
    if (buf.s != nullptr) {
        res->body.assign(ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
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
    res->body.assign(xml, xml_len);
    res->status_code = status;
    res->content_type = "application/xml; charset=utf-8";
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

static bool kislay_app_add_route(php_kislay_app_t *app, const std::string &method, const std::string &pattern, zval *handler) {
    if (!kislay_is_callable(handler)) {
        return false;
    }

    kislay::Route route;
    route.method = kislay_to_upper(method);
    std::string full_pattern = pattern;
    if (!app->group_stack.empty()) {
        std::string prefix;
        for (const auto &ctx : app->group_stack) {
            prefix = kislay_join_paths(prefix, ctx.prefix);
        }
        full_pattern = kislay_join_paths(prefix, pattern);
    }
    route.pattern = full_pattern;
    if (!kislay_build_route(full_pattern, &route)) {
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
    app->routes.push_back(std::move(route));
    return true;
}

PHP_METHOD(KislayApp, use) {
    zval *callable = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callable)
    ZEND_PARSE_PARAMETERS_END();

    if (!kislay_is_callable(callable)) {
        zend_throw_exception(zend_ce_exception, "Middleware must be callable", 0);
        RETURN_FALSE;
    }

    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    std::lock_guard<std::mutex> guard(app->lock);
    zval copy;
    ZVAL_COPY(&copy, callable);
    app->middleware.push_back(copy);
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
    std::lock_guard<std::mutex> guard(app->lock);
    if (!kislay_app_add_route(app, "OPTIONS", std::string(path, path_len), handler)) {
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
        zend_throw_exception(zend_ce_exception, "Failed to start server", 0);
        RETURN_FALSE;
    }

    app->running = true;
    while (app->running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    RETURN_TRUE;
}

PHP_METHOD(KislayApp, stop) {
    php_kislay_app_t *app = php_kislay_app_from_obj(Z_OBJ_P(getThis()));
    if (app->ctx != nullptr) {
        mg_stop(app->ctx);
        app->ctx = nullptr;
    }
    app->running = false;
    RETURN_TRUE;
}

PHP_METHOD(KislayApp, setMemoryLimit) {
    zend_long bytes = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    if (bytes < 0) {
        zend_throw_exception(zend_ce_exception, "Memory limit must be >= 0", 0);
        RETURN_FALSE;
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
    PHP_ME(KislayRequest, getParams, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getHeaders, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getHeader, arginfo_kislay_request_get_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, header, arginfo_kislay_request_header, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, input, arginfo_kislay_request_input, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, all, arginfo_kislay_request_get, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, has, arginfo_kislay_request_has, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, only, arginfo_kislay_request_only, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, setAttribute, arginfo_kislay_request_set_attribute, ZEND_ACC_PUBLIC)
    PHP_ME(KislayRequest, getAttribute, arginfo_kislay_request_get_attribute, ZEND_ACC_PUBLIC)
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
    PHP_ME(KislayResponse, send, arginfo_kislay_response_send, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendJson, arginfo_kislay_response_send_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, sendXml, arginfo_kislay_response_send_xml, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, json, arginfo_kislay_response_send_json, ZEND_ACC_PUBLIC)
    PHP_ME(KislayResponse, xml, arginfo_kislay_response_send_xml, ZEND_ACC_PUBLIC)
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
    PHP_ME(KislayApp, use, arginfo_kislay_app_use, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, get, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, post, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, put, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, patch, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, delete, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, options, arginfo_kislay_app_route, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, group, arginfo_kislay_app_group, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, setMemoryLimit, arginfo_kislay_app_set_memory_limit, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, getMemoryLimit, arginfo_kislay_app_get_memory_limit, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, enableGc, arginfo_kislay_app_enable_gc, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, listen, arginfo_kislay_app_listen, ZEND_ACC_PUBLIC)
    PHP_ME(KislayApp, stop, arginfo_kislay_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(kislayphp_extension) {
    REGISTER_INI_ENTRIES();
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "KislayPHP\\Core", "Request", kislay_request_methods);
    kislay_request_ce = zend_register_internal_class(&ce);
    kislay_request_ce->create_object = kislay_request_create_object;
    std::memcpy(&kislay_request_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_request_handlers.offset = XtOffsetOf(php_kislay_request_t, std);
    kislay_request_handlers.free_obj = kislay_request_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "KislayPHP\\Core", "Response", kislay_response_methods);
    kislay_response_ce = zend_register_internal_class(&ce);
    kislay_response_ce->create_object = kislay_response_create_object;
    std::memcpy(&kislay_response_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_response_handlers.offset = XtOffsetOf(php_kislay_response_t, std);
    kislay_response_handlers.free_obj = kislay_response_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "KislayPHP\\Core", "Next", kislay_next_methods);
    kislay_next_ce = zend_register_internal_class(&ce);
    kislay_next_ce->create_object = kislay_next_create_object;
    std::memcpy(&kislay_next_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_next_handlers.offset = XtOffsetOf(php_kislay_next_t, std);
    kislay_next_handlers.free_obj = kislay_next_free_obj;

    INIT_NS_CLASS_ENTRY(ce, "KislayPHP\\Core", "App", kislay_app_methods);
    kislay_app_ce = zend_register_internal_class(&ce);
    kislay_app_ce->create_object = kislay_app_create_object;
    std::memcpy(&kislay_app_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    kislay_app_handlers.offset = XtOffsetOf(php_kislay_app_t, std);
    kislay_app_handlers.free_obj = kislay_app_free_obj;

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

