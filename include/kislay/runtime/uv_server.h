#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <mutex>
#include <uv.h>
#include <llhttp.h>

#include "kislay/runtime/php_runtime.h"

namespace kislay::runtime {

struct UvConnection;

class UvServer {
public:
    // max_body_bytes: 0 means unlimited, matching the CivetWeb path's
    // app->max_body_bytes convention (kislay_extension.cpp).
    UvServer(std::string host, int port, std::unique_ptr<PhpRuntimePool>& pool,
             size_t max_body_bytes = 0);
    ~UvServer();

    bool start();
    void stop();

    // Used by PHP worker to queue responses and wake the loop
    void enqueue_response(UvConnection* conn, RuntimeResponseMessage response);

private:
    std::string host_;
    int port_;
    std::unique_ptr<PhpRuntimePool>& pool_;
    size_t max_body_bytes_;
    
    uv_loop_t* loop_;
    uv_tcp_t server_;
    uv_async_t async_wakeup_;
    std::atomic_bool running_{false};
    std::thread loop_thread_;

    // Lock-free-ish queue for cross-thread responses
    std::mutex response_mutex_;
    std::vector<std::pair<UvConnection*, RuntimeResponseMessage>> response_queue_;

    // Connections currently open at the libuv/OS level, guarded by the same
    // response_mutex_. A worker thread can still be computing a response
    // for a connection the client has since reset - process_responses()
    // must never dereference a UvConnection* it pops from response_queue_
    // without first confirming membership here, since on_close() (running
    // on the loop thread, independent of worker-thread timing) may have
    // already deleted it. See on_close()/process_responses() in
    // uv_server.cpp for the full race this closes.
    std::unordered_set<UvConnection*> live_connections_;

    // libuv callbacks
    static void on_new_connection(uv_stream_t *server, int status);
    static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
    static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
    static void on_close(uv_handle_t* handle);
    static void on_write_done(uv_write_t* req, int status);
    static void on_async_wakeup(uv_async_t* handle);

    // llhttp callbacks
    static int on_message_begin(llhttp_t* parser);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_headers_complete(llhttp_t* parser);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);

    void process_responses();
};

} // namespace kislay::runtime
