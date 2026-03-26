#include "kislay/runtime/uv_server.h"
#include <iostream>
#include <sstream>

namespace kislay::runtime {

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
    return 0;
}

int UvServer::on_body(llhttp_t* parser, const char* at, size_t length) {
    UvConnection* conn = static_cast<UvConnection*>(parser->data);
    conn->current_request.body.append(at, length);
    return 0;
}

struct UvCompletion : public RequestCompletion {
    UvConnection* conn;
    UvServer* server;
    
    UvCompletion(UvConnection* c, UvServer* s) : conn(c), server(s) {}
    
    void complete(RuntimeResponseMessage response) {
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
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

void UvServer::on_close(uv_handle_t* handle) {
    UvConnection* conn = static_cast<UvConnection*>(handle->data);
    delete conn;
}

void UvServer::on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    UvConnection* conn = static_cast<UvConnection*>(stream->data);
    if (nread > 0) {
        llhttp_execute(&conn->parser, buf->base, nread);
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
        std::cerr << "[uv-debug] New connection error: " << uv_strerror(status) << std::endl;
        return;
    }
    std::cout << "[uv-debug] Accepting new connection" << std::endl;

    UvServer* server = static_cast<UvServer*>(server_handle->data);
    UvConnection* conn = new UvConnection();
    conn->server = server;
    
    uv_tcp_init(server->loop_, &conn->handle);
    conn->handle.data = conn;
    
    if (uv_accept(server_handle, (uv_stream_t*)&conn->handle) == 0) {
        llhttp_settings_t settings;
        llhttp_settings_init(&settings);
        settings.on_message_begin = on_message_begin;
        settings.on_url = on_url;
        settings.on_header_field = on_header_field;
        settings.on_header_value = on_header_value;
        settings.on_headers_complete = on_headers_complete;
        settings.on_body = on_body;
        settings.on_message_complete = on_message_complete;
        
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
        
        std::ostringstream ss;
        ss << "HTTP/1.1 " << res.status_code << " OK\r\n";
        
        bool has_content_type = false;
        for (const auto& h : res.headers) {
            ss << h.first << ": " << h.second << "\r\n";
            if (h.first == "content-type") has_content_type = true;
        }
        if (!has_content_type && !res.content_type.empty()) {
            ss << "Content-Type: " << res.content_type << "\r\n";
        }
        ss << "Content-Length: " << res.body.size() << "\r\n";
        ss << "\r\n";
        ss << res.body;
        
        std::string raw = ss.str();
        WriteReq* wr = new WriteReq();
        wr->buf.base = (char*)malloc(raw.size());
        wr->buf.len = raw.size();
        memcpy(wr->buf.base, raw.data(), raw.size());
        wr->close_after_write = (res.status_code >= 500);
        
        uv_write(&wr->req, (uv_stream_t*)&conn->handle, &wr->buf, 1, on_write_done);
    }
}

void UvServer::on_async_wakeup(uv_async_t* handle) {
    UvServer* server = static_cast<UvServer*>(handle->data);
    server->process_responses();
}

// --- UvServer Implementation ---

UvServer::UvServer(std::string host, int port, std::unique_ptr<PhpRuntimePool>& pool)
    : host_(std::move(host)), port_(port), pool_(pool) {
    loop_ = uv_loop_new();
}

UvServer::~UvServer() {
    stop();
    uv_loop_delete(loop_);
}

bool UvServer::start() {
    if (running_.exchange(true)) return true;
    
    std::cout << "[uv-debug] Starting server on " << host_ << ":" << port_ << std::endl;
    std::cout << "[uv-debug] Spawning loop thread" << std::endl;
    
    // We must initialize the handles from the same thread that runs the loop
    // to avoid cross-thread handle management issues in libuv.
    loop_thread_ = std::thread([this]() {
        int r;
        r = uv_async_init(loop_, &async_wakeup_, on_async_wakeup);
        if (r < 0) { std::fprintf(stderr, "[uv-debug] async_init error: %s\n", uv_strerror(r)); fflush(stderr); return; }
        async_wakeup_.data = this;

        r = uv_tcp_init(loop_, &server_);
        if (r < 0) { std::fprintf(stderr, "[uv-debug] tcp_init error: %s\n", uv_strerror(r)); fflush(stderr); return; }
        server_.data = this;
        
        struct sockaddr_in addr;
        std::fprintf(stderr, "[uv-debug] Binding to %s:%d\n", host_.c_str(), port_); fflush(stderr);
        r = uv_ip4_addr(host_.c_str(), port_, &addr);
        if (r < 0) { std::fprintf(stderr, "[uv-debug] ip4_addr error: %s\n", uv_strerror(r)); fflush(stderr); return; }

        r = uv_tcp_bind(&server_, (const struct sockaddr*)&addr, 0);
        if (r < 0) { std::fprintf(stderr, "[uv-debug] bind error: %s\n", uv_strerror(r)); fflush(stderr); return; }
        
        r = uv_listen((uv_stream_t*)&server_, 10000, on_new_connection);
        if (r < 0) { std::fprintf(stderr, "[uv-debug] listen error: %s\n", uv_strerror(r)); fflush(stderr); return; }

        // Add dummy timer to keep loop alive
        uv_timer_t dummy;
        uv_timer_init(loop_, &dummy);
        uv_timer_start(&dummy, [](uv_timer_t*){}, 1000, 1000);

        std::fprintf(stderr, "[uv-debug] Loop thread started, entering uv_run. Server handle active: %d\n", uv_is_active((uv_handle_t*)&server_)); fflush(stderr);
        r = uv_run(loop_, UV_RUN_DEFAULT);
        std::fprintf(stderr, "[uv-debug] uv_run returned: %d\n", r); fflush(stderr);
        
        uv_timer_stop(&dummy);
        std::fprintf(stderr, "[uv-debug] Loop thread exiting\n"); fflush(stderr);
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