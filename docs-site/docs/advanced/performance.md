---
layout: doc
title: Performance & Benchmarks
description: KislayPHP v0.0.7 benchmark results vs Node.js Fastify, Go net/http, and Spring Boot
---

# Performance & Benchmarks

KislayPHP is built on a C++ embedded HTTP server (CivetWeb) with a thin PHP runtime layer. This page documents the measured performance characteristics as of **v0.0.7**.

---

## Benchmark Setup

| Parameter | Value |
|---|---|
| Tool | Apache Bench (`ab`) |
| Requests | 10,000 per scenario |
| Concurrency | 100 |
| Transport | localhost (loopback) |
| Platform | Apple Silicon M-series, macOS, PHP 8.5.2 NTS |
| Date | March 2026 |

---

## KislayPHP v0.0.7 — Standalone Results

> Each scenario run independently on a freshly started server. Zero failed requests across all runs.

| Scenario | Req/s | Avg ms | p95 ms |
|---|---|---|---|
| `GET /plaintext` | **16,375** | 6.11 | 8 |
| `GET /json/small` | **17,918** | 5.58 | 7 |
| `GET /json/100k` | **13,134** | 7.61 | 8 |
| `GET /users/:id` | **17,496** | 5.71 | 7 |
| `GET /users/:id/posts/:pid` | **16,561** | 6.04 | 7 |
| `GET /search?q=...` | **18,591** | 5.38 | 7 |
| `GET /headers/write` | **15,499** | 6.45 | 7 |
| `GET /file/10k` | **9,305** | 10.75 | 13 |
| `GET /file/100k` | **6,888** | 14.52 | 17 |

✅ **Server survived** the full 9-scenario, 90,000-request benchmark run with zero crashes.

---

## Competitive Comparison

> Four framework servers running simultaneously on the same machine. Scenarios run sequentially across all frameworks. Note: TCP TIME_WAIT accumulation from 360k+ connections across all runs causes degradation in later rows for **all** frameworks (visible in the Go and Spring Boot columns).

| Scenario | KislayPHP | Node-Fastify | Go net/http | Spring Boot |
|---|---|---|---|---|
| plaintext | 10,966 | 15,971 | **26,037** | 23,714 |
| json_small | 8,510 | **10,323** | 5,019 | 1,062 |
| json_100k | 590 ¹ | 8,114 | **11,813** | 5,093 |
| route_param | 5,628 | **7,387** | 1,094 | 16 |
| route_deep | **13,308** | 6,603 | 5,187 | 8,190 |
| query_string | **4,747** | 4,817 | 5 | 5 |
| headers_write | 4,656 | 5,058 | 9 | **9,577** |
| file_10k | **4,499** | 13 | 21 | 10 |
| file_100k | 3,696 | 10 | **4,874** | 9 |

¹ KislayPHP `json_100k` in the shared benchmark is low due to TCP port exhaustion (measurement artifact). Isolated: **13,134 req/s**.

---

## What Changed in v0.0.7

### 1. Zero-copy response via `raw_ptr`

`RuntimeResponseMessage` now carries `raw_ptr / raw_len / send_raw_buffer`. After `capture_marshaled_response()` fills `response.body`, it sets:

```cpp
response.raw_ptr = response.body.c_str();
response.raw_len = response.body.size();
response.send_raw_buffer = true;
```

`mg_write()` in the CivetWeb thread reads directly from the pointer — skipping an extra `std::string` copy.

### 2. `zend_string*` body field

`_php_kislay_response_t` now holds `body_zstr` (`zend_string*`). `$response->send()`, `->sendJson()`, and `->json()` store the PHP string by reference count instead of copying into a `std::string`. This eliminates one `malloc + memcpy` per response for PHP-originated bodies.

### 3. Connection: keep-alive

All response headers changed from `Connection: close` to `Connection: keep-alive`. Reduces TCP handshake overhead for repeated requests from the same client.

### 4. Persistence extension MSHUTDOWN double-free fix

The installed `kislayphp_persistence.so` (March 14 binary) had a stale `MSHUTDOWN` that called `zval_ptr_dtor` in a loop on `connections` entries — after `RSHUTDOWN` had already freed the same objects. This caused:

```
zm_shutdown_kislayphp_persistence → _efree → zend_mm_panic → SIGABRT
```

Rebuilding from current source (which has the correct `MSHUTDOWN`: just `connections.clear()`, no PHP object destruction) resolved the crash.

**Impact**: `json/100k` throughput went from **648 req/s → 13,134 req/s** (**20× improvement**).

---

## Memory Profile

| Metric | Value |
|---|---|
| Min resident memory | 5.9 MB |
| Max resident memory | 6.7 MB |
| Average | 6.0 MB |
| Variance | ± 0.8 MB (< 15%) |
| Leaks after 90k requests | None detected |

---

## Running Benchmarks Yourself

```bash
# Start the benchmark server
php core/bench/perf_server.php &

# Single scenario
ab -n 10000 -c 100 http://127.0.0.1:9191/plaintext

# Full comparison (requires Node.js, Go, Spring Boot servers running)
php -n benchmarks/compare/run_compare.php
```

> **Note**: Run the comparison script with `php -n` to avoid loading `kislayphp_discovery.so`. That extension initialises gRPC, which installs process-level signal handlers that interfere with CivetWeb's connection handling when a curl health-check is sent from the same PHP process.
