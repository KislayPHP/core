#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kislay::runtime {

// Flat, cache-friendly alternative to unordered_map<string,string> for HTTP
// headers.  libc++ unordered_map allocates one heap node per entry; for the
// typical 8-20 header case that is 8-20 extra malloc calls per request.
// FlatHeaders stores pairs in a single contiguous vector — 1 allocation for
// the buffer instead of N+1.  find() is O(N) linear scan but for N≤20 with
// mostly-SSO keys the memory locality beats the hash map.
class FlatHeaders {
public:
    using value_type = std::pair<std::string, std::string>;
    using container  = std::vector<value_type>;
    using iterator       = container::iterator;
    using const_iterator = container::const_iterator;

    void reserve(std::size_t n) { data_.reserve(n); }
    void clear()                { data_.clear(); }
    bool empty() const          { return data_.empty(); }
    std::size_t size() const    { return data_.size(); }

    iterator       begin()        { return data_.begin(); }
    iterator       end()          { return data_.end(); }
    const_iterator begin()  const { return data_.begin(); }
    const_iterator end()    const { return data_.end(); }
    const_iterator cbegin() const { return data_.cbegin(); }
    const_iterator cend()   const { return data_.cend(); }

    iterator find(const std::string &key) {
        for (auto it = data_.begin(); it != data_.end(); ++it)
            if (it->first == key) return it;
        return data_.end();
    }
    const_iterator find(const std::string &key) const {
        for (auto it = data_.begin(); it != data_.end(); ++it)
            if (it->first == key) return it;
        return data_.end();
    }

    std::size_t count(const std::string &key) const {
        return find(key) != data_.end() ? 1 : 0;
    }

    std::string &operator[](const std::string &key) {
        for (auto &p : data_)
            if (p.first == key) return p.second;
        data_.push_back({key, {}});
        return data_.back().second;
    }

    const std::string &at(const std::string &key) const {
        for (const auto &p : data_)
            if (p.first == key) return p.second;
        throw std::out_of_range("FlatHeaders::at: key not found");
    }

private:
    container data_;
};

using TaskId = std::uint64_t;
using PhpTaskId = std::uint64_t;

class RequestCompletion;

struct HttpRequestTask {
    TaskId task_id{0};
    std::size_t owner_lane{0};
    std::string method;
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    long timeout_ms{10000};
    int max_retries{0};
    int retry_delay_ms{0};
};

struct HttpResultMessage {
    TaskId task_id{0};
    std::size_t owner_lane{0};
    bool ok{false};
    long response_code{0};
    std::string response_body;
    std::string error_message;
};

struct PhpTaskMessage {
    PhpTaskId task_id{0};
    std::size_t owner_lane{0};
};

struct RuntimeRequestMessage {
    TaskId task_id{0};
    std::string method;
    std::string uri;
    std::string route_uri;
    std::string query;
    std::string body;
    FlatHeaders headers;
    std::string remote_addr;
    std::shared_ptr<RequestCompletion> completion;
};

struct RuntimeResponseMessage {
    TaskId task_id{0};
    long status_code{200};
    std::string body;
    // Option 1: zero-copy send path — raw_ptr points into body.c_str() after body is filled.
    // kislay_send_marshaled_response uses raw_ptr/raw_len directly for mg_write, bypassing
    // any further std::string indirection.
    const char *raw_ptr{nullptr};
    std::size_t raw_len{0};
    bool send_raw_buffer{false};
    std::string file_path;
    std::string content_type{"text/plain"};
    std::unordered_map<std::string, std::string> headers;
    bool send_file{false};
    std::string request_id;
    std::string traceparent;
    std::string tracestate;
    std::string request_error;

    RuntimeResponseMessage() = default;

    RuntimeResponseMessage(const RuntimeResponseMessage &other)
        : task_id(other.task_id)
        , status_code(other.status_code)
        , body(other.body)
        , raw_ptr(nullptr)
        , raw_len(0)
        , send_raw_buffer(other.send_raw_buffer)
        , file_path(other.file_path)
        , content_type(other.content_type)
        , headers(other.headers)
        , send_file(other.send_file)
        , request_id(other.request_id)
        , traceparent(other.traceparent)
        , tracestate(other.tracestate)
        , request_error(other.request_error) {
        refresh_raw_buffer_view();
    }

    RuntimeResponseMessage &operator=(const RuntimeResponseMessage &other) {
        if (this == &other) {
            return *this;
        }
        task_id = other.task_id;
        status_code = other.status_code;
        body = other.body;
        send_raw_buffer = other.send_raw_buffer;
        file_path = other.file_path;
        content_type = other.content_type;
        headers = other.headers;
        send_file = other.send_file;
        request_id = other.request_id;
        traceparent = other.traceparent;
        tracestate = other.tracestate;
        request_error = other.request_error;
        refresh_raw_buffer_view();
        return *this;
    }

    RuntimeResponseMessage(RuntimeResponseMessage &&other) noexcept
        : task_id(other.task_id)
        , status_code(other.status_code)
        , body(std::move(other.body))
        , raw_ptr(nullptr)
        , raw_len(0)
        , send_raw_buffer(other.send_raw_buffer)
        , file_path(std::move(other.file_path))
        , content_type(std::move(other.content_type))
        , headers(std::move(other.headers))
        , send_file(other.send_file)
        , request_id(std::move(other.request_id))
        , traceparent(std::move(other.traceparent))
        , tracestate(std::move(other.tracestate))
        , request_error(std::move(other.request_error)) {
        refresh_raw_buffer_view();
        other.clear_raw_buffer_view();
    }

    RuntimeResponseMessage &operator=(RuntimeResponseMessage &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        task_id = other.task_id;
        status_code = other.status_code;
        body = std::move(other.body);
        send_raw_buffer = other.send_raw_buffer;
        file_path = std::move(other.file_path);
        content_type = std::move(other.content_type);
        headers = std::move(other.headers);
        send_file = other.send_file;
        request_id = std::move(other.request_id);
        traceparent = std::move(other.traceparent);
        tracestate = std::move(other.tracestate);
        request_error = std::move(other.request_error);
        refresh_raw_buffer_view();
        other.clear_raw_buffer_view();
        return *this;
    }

    void refresh_raw_buffer_view() {
        if (send_raw_buffer && !body.empty()) {
            raw_ptr = body.c_str();
            raw_len = body.size();
        } else {
            clear_raw_buffer_view();
        }
    }

    void clear_raw_buffer_view() {
        raw_ptr = nullptr;
        raw_len = 0;
        send_raw_buffer = false;
    }
};

} // namespace kislay::runtime
