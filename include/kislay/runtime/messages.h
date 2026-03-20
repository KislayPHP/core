#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace kislay::runtime {

using TaskId = std::uint64_t;
using PhpTaskId = std::uint64_t;

class RequestCompletion;

struct HttpRequestTask {
    TaskId task_id{0};
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
    bool ok{false};
    long response_code{0};
    std::string response_body;
    std::string error_message;
};

struct RuntimeRequestMessage {
    TaskId task_id{0};
    std::string method;
    std::string uri;
    std::string route_uri;
    std::string query;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string remote_addr;
    std::shared_ptr<RequestCompletion> completion;
};

struct RuntimeResponseMessage {
    TaskId task_id{0};
    long status_code{200};
    std::string body;
    std::string file_path;
    std::string content_type{"text/plain"};
    std::unordered_map<std::string, std::string> headers;
    bool send_file{false};
    std::string request_id;
    std::string traceparent;
    std::string tracestate;
    std::string request_error;
};

} // namespace kislay::runtime
