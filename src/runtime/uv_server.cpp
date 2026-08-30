#include "kislay/runtime/uv_server.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace kislay::runtime {

static const char* kislay_uv_status_text(int c) {
    switch(c) {
        case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted";
        case 204: return "No Content"; case 301: return "Moved Permanently";
        case 302: return "Found"; case 304: return "Not Modified";
        case 400: return "Bad Request"; case 401: return "Unauthorized";
        case 403: return "Forbidden"; case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 409: return "Conflict";
        case 410: return "Gone"; case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests"; case 500: return "Internal Server Error";
        case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout"; default: return "Unknown";
    }
}

struct UvConnection {
    uv_tcp_t handle;
    llhttp_t parser;
    UvServer* server;
    
    // Request building state
    RuntimeRequestMessage current_request;
    std::string current_header_field;
    std::string current_header_value;
    bool last_was_value{false};
    
    // Reference counting / lifecycle
    bool active{true};

    // Set from the request's "Connection" header in on_headers_complete,
    // before current_request is moved out to the worker pool in
    // on_message_complete (so process_responses(), which only has `conn`
    // and no access to the original request, can still see it at write
    // time). Previously nothing here ever closed a non-5xx connection - a
    // client sending "Connection: close" (as any single-shot client, and
    // every test in tests/server_helper.inc's make_request(), does) would
    // hang waiting for EOF that would never come.
    bool client_wants_close{false};
};

// --- llhttp callbacks ---
int UvServer::on_message_begin(llhttp_t* parser) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    conn->current_request = RuntimeRequestMessage{};
    conn->current_header_field.clear();
    conn->current_header_value.clear();
    conn->last_was_value = false;
    return 0;
}

int UvServer::on_url(llhttp_t* parser, const char* at, size_t length) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    conn->current_request.uri.append(at, length);
    return 0;
}

int UvServer::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    if (conn->last_was_value) {
        // Save previous header
        if (!conn->current_header_field.empty()) {
            std::string field = conn->current_header_field;
            for (auto &c : field) c = std::tolower(c);
            conn->current_request.headers[field] = conn->current_header_value;
        }
        conn->current_header_field.clear();
        conn->current_header_value.clear();
    }
    conn->current_header_field.append(at, length);
    conn->last_was_value = false;
    return 0;
}

int UvServer::on_header_value(llhttp_t* parser, const char* at, size_t length) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    conn->current_header_value.append(at, length);
    conn->last_was_value = true;
    return 0;
}

int UvServer::on_headers_complete(llhttp_t* parser) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    if (!conn->current_header_field.empty()) {
        std::string field = conn->current_header_field;
        for (auto &c : field) c = std::tolower(c);
        conn->current_request.headers[field] = conn->current_header_value;
    }
    conn->current_request.method = llhttp_method_name((llhttp_method_t)parser->method);

    // Recorded on the connection itself (not just current_request) because
    // current_request is moved out to the worker pool in
    // on_message_complete, and process_responses() - where the write
    // actually happens - only has the UvConnection*, not the original
    // request.
    auto it = conn->current_request.headers.find("connection");
    if (it != conn->current_request.headers.end()) {
        std::string value = it->second;
        for (auto &c : value) c = std::tolower(c);
        if (value.find("close") != std::string::npos) {
            conn->client_wants_close = true;
        }
    }
    return 0;
}

int UvServer::on_body(llhttp_t* parser, const char* at, size_t length) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    // Mirrors the CivetWeb path's app->max_body_bytes guard
    // (kislay_extension.cpp) - checked cumulatively here (rather than off a
    // declared Content-Length up front) so it also catches chunked-encoded
    // bodies with no declared length. Returning a non-zero errno stops
    // llhttp_execute() from calling further callbacks and makes it return
    // that errno to on_read(), which closes the connection.
    if (conn->server->max_body_bytes_ > 0 &&
        conn->current_request.body.size() + length > conn->server->max_body_bytes_) {
        return HPE_USER;
    }
    conn->current_request.body.append(at, length);
    return 0;
}

struct UvCompletion : public RequestCompletion {
    UvConnection* conn;
    UvServer* server;
    
    UvCompletion(UvConnection* c, UvServer* s) : conn(c), server(s) {}

    void complete(RuntimeResponseMessage response) override {
        // Queue the response and wake the libuv loop
        server->enqueue_response(conn, std::move(response));
    }
};

int UvServer::on_message_complete(llhttp_t* parser) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);

    // Parse query params out of URI
    size_t qpos = conn->current_request.uri.find('?');
    if (qpos != std::string::npos) {
        conn->current_request.query = conn->current_request.uri.substr(qpos + 1);
        conn->current_request.route_uri = conn->current_request.uri.substr(0, qpos);
    } else {
        conn->current_request.route_uri = conn->current_request.uri;
    }
    
    // Dispatch to PHP Runtime Pool
    auto req = std::move(conn->current_request);
    req.completion = std::make_shared<UvCompletion>(conn, conn->server);
    
    if (!conn->server->pool_->submit(std::move(req))) {
        // Queue full, 503
        RuntimeResponseMessage res;
        res.status_code = 503;
        res.body = "Service Unavailable (Queue Full)";
        conn->server->enqueue_response(conn, std::move(res));
    }
    return 0;
}

// --- libuv callbacks ---

void UvServer::on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    const size_t cap = suggested_size < 65536 ? suggested_size : 65536;
    buf->base = (char*)malloc(cap);
    buf->len = buf->base ? cap : 0;
}

void UvServer::on_close(uv_handle_t* handle) {
    UvConnection* conn = static_cast<UvConnection*>(handle->data);

    // A worker thread can be mid-handler for this connection - holding a
    // UvCompletion built from this exact `conn` pointer - when the client
    // resets it. uv_close()/on_close() run on this loop thread regardless
    // of what the worker thread is doing, so `delete conn` below can (and
    // under real concurrent load, reliably does) run well before that
    // worker later calls complete() -> enqueue_response(conn, ...) with a
    // pointer that's already dangling by then. Checking a flag on `conn`
    // itself at write time isn't safe either - it's the same
    // read-through-a-maybe-freed-pointer hazard, just moved. Confirmed via
    // a real crash under concurrent resets: libuv's own
    // uv__check_before_write assertion firing because conn->handle.type
    // read as garbage from freed memory (SIGABRT, "does not yet support
    // other types of streams").
    //
    // live_connections_ (checked in process_responses() before it ever
    // touches `conn`) is the actual safety gate - erase from it here,
    // before delete, while `conn` is still guaranteed valid. This is safe
    // regardless of whether the worker's enqueue_response() call for this
    // connection happens before or after this erase, since
    // process_responses() never dereferences `conn` until it has already
    // confirmed membership in this always-valid set.
    {
        std::lock_guard<std::mutex> lock(conn->server->response_mutex_);
        conn->server->live_connections_.erase(conn);
    }

    delete conn;
}

void UvServer::on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    UvConnection* conn = static_cast<UvConnection*>(stream->data);
    if (nread > 0) {
        llhttp_errno_t err = llhttp_execute(&conn->parser, buf->base, nread);
        if (err != HPE_OK && conn->active) {
            // Malformed request or an on_body/on_* callback signaled abort
            // (e.g. the max_body_bytes cap above) - this return value used
            // to be discarded entirely, leaving the connection open forever
            // with no response and no further reads. Close it instead.
            conn->active = false;
            if (buf->base) free(buf->base);
            uv_close((uv_handle_t*)stream, on_close);
            return;
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            std::cerr << "Read error: " << uv_err_name(nread) << std::endl;
        }
        uv_close((uv_handle_t*)stream, on_close);
    }
    if (buf->base) free(buf->base);
}

void UvServer::on_new_connection(uv_stream_t *server_handle, int status) {
    if (status < 0) {
#ifdef KISLAY_DEBUG
        std::cerr << "[uv-debug] New connection error: " << uv_strerror(status) << std::endl;
#endif
        return;
    }
#ifdef KISLAY_DEBUG
    std::cout << "[uv-debug] Accepting new connection" << std::endl;
#endif

    UvServer* server = static_cast<UvServer*>(server_handle->data);
    UvConnection* conn = new UvConnection();
    conn->server = server;
    {
        std::lock_guard<std::mutex> lock(server->response_mutex_);
        server->live_connections_.insert(conn);
    }

    uv_tcp_init(server->loop_, &conn->handle);
    conn->handle.data = conn;
    
    if (uv_accept(server_handle, (uv_stream_t*)&conn->handle) == 0) {
        // llhttp_init() only stores the *pointer* it's given
        // (parser->settings = (void*) settings - see third_party/llhttp/src/api.c)
        // rather than copying the struct. `settings` here used to be a
        // stack-local of this function - valid on entry, but on_new_connection()
        // returns immediately after uv_read_start(), so by the time on_read()
        // later calls llhttp_execute() (a separate, later callback invocation
        // with its own stack frame) on_read() dereferences conn->parser.settings
        // into memory that used to be this function's stack frame, long since
        // reused by other calls. Every connection accepted by this server was
        // reading callback pointers out of a dangling stack slot - apparently
        // "worked" on macOS/clang/arm64 (the stale stack memory happened to
        // still look like valid function pointers long enough), but crashed
        // reliably and immediately, on the very first request of any kind, on
        // Linux/gcc/arm64 (confirmed via gdb: SIGSEGV in llhttp__on_url_complete
        // jumping to a garbage address read from the stale stack slot). All
        // connections share the exact same callback set, so hoist it to a
        // single static instance built once (C++11 guarantees thread-safe
        // static-local init) instead of rebuilding a short-lived one per
        // connection.
        static const llhttp_settings_t settings = [] {
            llhttp_settings_t s;
            llhttp_settings_init(&s);
            s.on_message_begin = on_message_begin;
            s.on_url = on_url;
            s.on_header_field = on_header_field;
            s.on_header_value = on_header_value;
            s.on_headers_complete = on_headers_complete;
            s.on_body = on_body;
            s.on_message_complete = on_message_complete;
            return s;
        }();

        llhttp_init(&conn->parser, HTTP_REQUEST, &settings);
        conn->parser.data = conn;
        
        uv_read_start((uv_stream_t*)&conn->handle, on_alloc, on_read);
    } else {
        uv_close((uv_handle_t*)&conn->handle, on_close);
    }
}

struct WriteReq {
    uv_write_t req;
    uv_buf_t buf;
    bool close_after_write;
};

void UvServer::on_write_done(uv_write_t* req, int status) {
    WriteReq* wr = reinterpret_cast<WriteReq*>(req);
    free(wr->buf.base);
    if (wr->close_after_write && req->handle) {
        if (!uv_is_closing((uv_handle_t*)req->handle)) {
            uv_close((uv_handle_t*)req->handle, on_close);
        }
    }
    delete wr;
}

void UvServer::process_responses() {
    std::vector<std::pair<UvConnection*, RuntimeResponseMessage>> queue;
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        queue.swap(response_queue_);
    }

    for (auto& pair : queue) {
        UvConnection* conn = pair.first;
        RuntimeResponseMessage& res = pair.second;

        // conn may already be freed (client reset the connection while a
        // worker thread was still computing this response - see the
        // matching comment in on_close()) - live_connections_ is the only
        // thing we're allowed to touch before proving conn is still valid;
        // never dereference conn itself until this check passes.
        {
            std::lock_guard<std::mutex> lock(response_mutex_);
            if (live_connections_.find(conn) == live_connections_.end()) {
                continue;
            }
        }

        // Response::sendFile() (kislay_extension.cpp) sets send_file/file_path
        // and clears body, relying on the backend to stream the file. The
        // CivetWeb backend does that via mg_send_file_body(); this backend
        // previously had no equivalent at all, so a libuv response to
        // sendFile() always serialized an empty body - read the file here.
        // In-memory read (matching res.body being an in-memory std::string
        // everywhere else in this backend already) rather than a true
        // streamed/chunked write; fine for the file sizes this framework's
        // own examples/benchmarks serve, but a very large file would be
        // read fully into memory before the first byte is written.
        std::string file_body;
        const std::string* body = &res.body;
        if (res.send_file && !res.file_path.empty()) {
            std::ifstream file(res.file_path, std::ios::binary);
            if (file) {
                std::ostringstream ss;
                ss << file.rdbuf();
                file_body = ss.str();
            }
            body = &file_body;
        }

        // Build response header into a flat std::string to avoid ostringstream overhead.
        // Body is appended directly — no separate malloc/memcpy for header+body merge.
        std::string raw;
        raw.reserve(128 + body->size());
        raw += "HTTP/1.1 ";
        raw += std::to_string(res.status_code);
        raw += ' ';
        raw += kislay_uv_status_text(res.status_code);
        raw += "\r\n";

        bool has_content_type = false;
        for (const auto& h : res.headers) {
            // Content-Length is always written below from the actual body
            // we're about to send (res->headers may carry a stale/advisory
            // one, e.g. from sendFile() - see above) - skip it here so we
            // never emit two Content-Length header lines for one response.
            if (h.first == "content-length") continue;
            raw += h.first;
            raw += ": ";
            raw += h.second;
            raw += "\r\n";
            if (h.first == "content-type") has_content_type = true;
        }
        if (!has_content_type && !res.content_type.empty()) {
            raw += "Content-Type: ";
            raw += res.content_type;
            raw += "\r\n";
        }
        raw += "Content-Length: ";
        raw += std::to_string(body->size());
        raw += "\r\n\r\n";
        raw += *body;

        WriteReq* wr = new WriteReq();
        wr->buf.base = (char*)malloc(raw.size());
        wr->buf.len = raw.size();
        memcpy(wr->buf.base, raw.data(), raw.size());
        // Previously only 5xx responses closed the connection afterward, so
        // a client that sent "Connection: close" (as any single-shot
        // client reasonably does) would sit waiting for EOF that would
        // never arrive - the connection stayed open indefinitely.
        wr->close_after_write = (res.status_code >= 500) || conn->client_wants_close;

        // uv_write()'s return value used to be discarded. A negative return
        // means it failed SYNCHRONOUSLY, in which case libuv guarantees
        // on_write_done is never invoked - so wr/wr->buf.base would leak on
        // every such failure. This is reachable, not just theoretical: the
        // live_connections_ check above only proves `conn` hasn't been
        // *deleted* yet (on_close() hasn't run), not that its handle isn't
        // already *closing* - uv_close() marks a handle closing immediately
        // when called (e.g. from on_read() on a parse error on a later
        // pipelined request on this same connection), but the matching
        // on_close() completion - which is what erases live_connections_ -
        // only runs on a later loop tick. A response for an earlier,
        // still-in-flight request on that same connection can land here via
        // process_responses() in that window, findslive_connections_ still
        // still has it, and then hits uv_write() on an already-closing handle,
        // which fails synchronously.
        int wr_status = uv_write(&wr->req, (uv_stream_t*)&conn->handle, &wr->buf, 1, on_write_done);
        if (wr_status < 0) {
            free(wr->buf.base);
            delete wr;
        }
    }
}

void UvServer::on_async_wakeup(uv_async_t* handle) {
    UvServer* server = static_cast<UvServer*>(handle->data);
    server->process_responses();
}

// --- UvServer Implementation ---

UvServer::UvServer(std::string host, int port, std::unique_ptr<PhpRuntimePool>& pool,
                    size_t max_body_bytes)
    : host_(std::move(host)), port_(port), pool_(pool), max_body_bytes_(max_body_bytes) {
    loop_ = uv_loop_new();
}

UvServer::~UvServer() {
    stop();
    uv_loop_delete(loop_);
}

bool UvServer::start() {
    if (running_.exchange(true)) return true;
    
#ifdef KISLAY_DEBUG
    std::cout << "[uv-debug] Starting server on " << host_ << ":" << port_ << std::endl;
#endif
#ifdef KISLAY_DEBUG
    std::cout << "[uv-debug] Spawning loop thread" << std::endl;
#endif
    
    // We must initialize the handles from the same thread that runs the loop
    // to avoid cross-thread handle management issues in libuv.
    loop_thread_ = std::thread([this]() {
        int r;
        r = uv_async_init(loop_, &async_wakeup_, on_async_wakeup);
        if (r < 0) {
#ifdef KISLAY_DEBUG
            std::fprintf(stderr, "[uv-debug] async_init error: %s\n", uv_strerror(r)); fflush(stderr);
#endif
            return;
        }
        async_wakeup_.data = this;

        r = uv_tcp_init(loop_, &server_);
        if (r < 0) {
#ifdef KISLAY_DEBUG
            std::fprintf(stderr, "[uv-debug] tcp_init error: %s\n", uv_strerror(r)); fflush(stderr);
#endif
            return;
        }
        server_.data = this;

        struct sockaddr_in addr;
#ifdef KISLAY_DEBUG
        std::fprintf(stderr, "[uv-debug] Binding to %s:%d\n", host_.c_str(), port_); fflush(stderr);
#endif
        r = uv_ip4_addr(host_.c_str(), port_, &addr);
        if (r < 0) {
#ifdef KISLAY_DEBUG
            std::fprintf(stderr, "[uv-debug] ip4_addr error: %s\n", uv_strerror(r)); fflush(stderr);
#endif
            return;
        }

        r = uv_tcp_bind(&server_, (const struct sockaddr*)&addr, 0);
        if (r < 0) {
#ifdef KISLAY_DEBUG
            std::fprintf(stderr, "[uv-debug] bind error: %s\n", uv_strerror(r)); fflush(stderr);
#endif
            return;
        }

        r = uv_listen((uv_stream_t*)&server_, 10000, on_new_connection);
        if (r < 0) {
#ifdef KISLAY_DEBUG
            std::fprintf(stderr, "[uv-debug] listen error: %s\n", uv_strerror(r)); fflush(stderr);
#endif
            return;
        }

        // Add dummy timer to keep loop alive
        uv_timer_t dummy;
        uv_timer_init(loop_, &dummy);
        uv_timer_start(&dummy, [](uv_timer_t*){}, 1000, 1000);
#ifdef KISLAY_DEBUG
        std::fprintf(stderr, "[uv-debug] Loop thread started, entering uv_run. Server handle active: %d\n", uv_is_active((uv_handle_t*)&server_)); fflush(stderr);
#endif
        r = uv_run(loop_, UV_RUN_DEFAULT);
#ifdef KISLAY_DEBUG
        std::fprintf(stderr, "[uv-debug] uv_run returned: %d\n", r); fflush(stderr);
#endif

        uv_timer_stop(&dummy);
#ifdef KISLAY_DEBUG
        std::fprintf(stderr, "[uv-debug] Loop thread exiting\n"); fflush(stderr);
#endif
    });
    
    return true;
}

void UvServer::stop() {
    if (!running_.exchange(false)) return;
    
    // Signal loop to stop
    uv_stop(loop_);
    uv_async_send(&async_wakeup_);
    
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
}

void UvServer::enqueue_response(UvConnection* conn, RuntimeResponseMessage response) {
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        response_queue_.push_back({conn, std::move(response)});
    }
    uv_async_send(&async_wakeup_);
}

} // namespace kislay::runtime
