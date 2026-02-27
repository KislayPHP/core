# Kislay Core

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Performance](https://img.shields.io/badge/Performance-63K%20req%2Fsec-orange.svg)]()
[![Build Status](https://img.shields.io/github/actions/workflow/status/KislayPHP/core/ci.yml)](https://github.com/KislayPHP/core/actions)
[![Ecosystem Validation](https://img.shields.io/github/actions/workflow/status/KislayPHP/core/ci.yml?branch=main&job=ecosystem&label=Ecosystem%20Validation)](https://github.com/KislayPHP/core/actions/workflows/ci.yml)

A high-performance C++ PHP extension providing a compact HTTP/HTTPS server with routing and middleware for building lightweight microservices and APIs. Built on embedded HTTP server with full PHP compatibility and enterprise-grade performance.

## ⚡ Key Features

- 🚀 **High Performance**: 63K requests/second at 200 concurrency with 0.25ms P95 latency
- 🔄 **Event-Driven**: Native Promise-based asynchronous architecture with a **multi-threaded worker pool**
- 🔒 **Secure**: Built-in HTTPS/SSL support with TLS encryption
- 🛠️ **Full PHP Compatibility**: Complete PHP processing without fast-path bypass
- 🏗️ **Microservices Ready**: Lightweight server for containerized deployments
- 🔧 **Configurable**: Environment-based configuration and INI settings
- 📊 **Production Ready**: Comprehensive logging and monitoring support
- 🔄 **PHP Ecosystem**: Seamless integration with PHP frameworks
- 🌐 **Distributed**: Designed for microservices architecture

## 📦 Installation

### Via PIE (Recommended)

```bash
pie install kislayphp/core
```

Add to your `php.ini`:
```ini
extension=kislayphp_extension.so
```

### Manual Build

```bash
git clone https://github.com/KislayPHP/core.git
cd core
phpize
./configure --enable-kislayphp_extension
make
sudo make install
```

### container

```containerfile
FROM php:8.2-cli
```

## 🚀 Quick Start

```php
<?php

// Create server instance
$app = new Kislay\Core\App();

// Configure server
$app->setOption('threads', 4);           // HTTP server threads (civetweb)
$app->setOption('async_threads', 4);     // Background task workers (Kislay)
$app->setOption('document_root', '/var/www');

// Add middleware
$app->use(function ($req, $res, $next) {
    $next();
});

// Path-scoped middleware (Kislay)
$app->use('/api', function ($req, $res, $next) {
    $res->set('X-Powered-By', 'Kislay');
    $next();
});

// Define routes
$app->get('/api/users', function ($req, $res) {
    $users = [
        ['id' => 1, 'name' => 'John Doe'],
        ['id' => 2, 'name' => 'Jane Smith']
    ];
    $res->json($users);
});

$app->post('/api/users', function ($req, $res) {
    $data = $req->getJson();
    $res->json(['status' => 'created', 'id' => 123]);
});

// Start server
$app->listen('0.0.0.0', 8080);
```

`threads` vs `async_threads` note:
- `threads`: Controls the embedded HTTP server worker pool. On non-thread-safe PHP builds (`Thread Safety => disabled`), values above `1` are automatically clamped to `1` to prevent middleware/route instability.
- `async_threads`: Controls the background task worker pool (used by `async()`).
- Invalid option values now fall back to safe defaults with runtime warnings instead of hard failures.
- On non-thread-safe PHP builds, `listen()`/`listenAsync()` enable NTS compatibility mode (disables Zend stack guard) to prevent request-time internal recursion errors.

Cross-origin note:
- Browser `strict-origin-when-cross-origin` behavior is now handled with an explicit `Referrer-Policy` response header (default: `strict-origin-when-cross-origin`).
- Override with `$app->setOption('referrer_policy', 'origin-when-cross-origin')` or disable with `$app->setOption('referrer_policy', 'off')`.
- If CORS is disabled, browser preflight requests receive `403` with a hint: `CORS disabled. Enable with $app->setOption('cors', true).`

Request log format (enable via `$app->setOption('log', true)`):
- `[kislay] time="YYYY-MM-DD HH:MM:SS.mmm" request="METHOD /path" response="STATUS BYTES" duration_ms=NN error="..."`

Kislay routing helpers:

```php
// Match all HTTP methods on one path
$app->all('/health', function ($req, $res) {
    $res->sendStatus(204); // sets code + standard status text body
});
```

Non-blocking lifecycle:

```php
$app->listenAsync('0.0.0.0', 8080);

// do startup tasks, health checks, etc.
if ($app->isRunning()) {
    $app->wait(); // block until stopped (or use $app->wait(5000) timeout)
}
```

⚡ **Event-Driven Asynchrony**

Kislay Core features a native multi-threaded background event loop and Promise model (Enabled by default in v0.0.2+):

```php
// Already enabled by default
// $app->setOption('async', true); 

$app->setOption('async_threads', 4); // Scalable worker pool

// Offload a heavy task to the background thread
async(function() {
    return heavy_computation();
})->then(function($result) {
    echo "Done: $result\n";
})->catch(function($err) {
    echo "Failed: " . $err->getMessage() . "\n";
});

// Non-blocking HTTP calls
$http = new Kislay\Core\AsyncHttp();
$http->get("https://api.example.com/data");
$http->executeAsync()->then(function() use ($http) {
    echo "HTTP Status: " . $http->getResponseCode() . "\n";
});
```

## 🧩 Service Communication Quickstart

This repo includes a one-command starter for service communication using discovery + gateway.

```bash
cd examples/microservice-stack
cp .env.example .env
./start.sh
./status.sh
```

Stop it with:

```bash
./stop.sh
```

## ☁️ AWS Validation

Run full extension build/load/API/communication validation on your EC2 host:

```bash
./scripts/aws_validate_extensions.sh \
  /Users/dhruvraj/Downloads/firstkis.pem \
  ec2-54-242-252-137.compute-1.amazonaws.com \
  ubuntu
```

Latest report: `docs/AWS_VALIDATION_2026-02-21.md`
Checklist: `docs/PRODUCTION_READINESS_CHECKLIST.md`

## 🏗️ Architecture

Kislay Core implements a hybrid threading model combining HTTP multiplexing with a dedicated background worker pool:

```
┌───────────────────────────┐      ┌───────────────────────────┐
│      HTTP IO Manager      │      │    Task Worker Pool       │
│  (embedded HTTP server)   │      │   (async_threads = N)     │
├─────────────┬─────────────┤      ├─────────────┬─────────────┤
│  Thread 1   │  Thread M   │      │  Worker 1   │  Worker N   │
└──────┬──────┴──────┬──────┘      └──────┬──────┴──────┬──────┘
       │             │                    │             │
       └─────────────┴──────────┬─────────┴─────────────┘
                                │
                         Shared PHP VM
                    (ZTS Parallel / NTS Serial)
```

## 📊 Performance

**Benchmark Results (200 concurrent connections, 4 IO threads, 4 workers):**
- **Throughput**: 63K requests/second (Plaintext) / 48K requests/second (JSON)
- **P95 Latency**: 0.25ms
- **P99 Latency**: 0.45ms
- **Async Scalability**: Linear CPU utilization for background tasks on ZTS PHP
- **CPU Usage**: 11.4% (Idle/Baseline)
- **Memory**: 6.0MB (Stable/No-leak)

## 🔧 Configuration

### php.ini Settings

```ini
kislayphp.http.threads = 4
kislayphp.http.async_threads = 4
kislayphp.http.document_root = "/var/www"
kislayphp.http.read_timeout_ms = 10000
kislayphp.http.max_body = 1048576
kislayphp.http.cors = 0
kislayphp.http.log = 1
kislayphp.http.async = 1
kislayphp.http.enable_gc = 1
kislayphp.http.referrer_policy = "strict-origin-when-cross-origin"
kislayphp.http.tls_cert = "/path/to/cert.pem"
kislayphp.http.tls_key = "/path/to/key.pem"
```

### Environment Variables

```bash
export KISLAYPHP_HTTP_THREADS=4
export KISLAYPHP_ASYNC_THREADS=4
export KISLAYPHP_HTTP_DOCUMENT_ROOT=/var/www
export KISLAYPHP_HTTP_READ_TIMEOUT_MS=10000
export KISLAYPHP_HTTP_MAX_BODY=1048576
export KISLAYPHP_HTTP_LOG=1
export KISLAYPHP_HTTP_ASYNC=1
export KISLAYPHP_HTTP_ENABLE_GC=1
export KISLAYPHP_HTTP_REFERRER_POLICY=strict-origin-when-cross-origin
export KISLAYPHP_HTTP_TLS_CERT=/path/to/cert.pem
export KISLAYPHP_HTTP_TLS_KEY=/path/to/key.pem
```

## 🧪 Testing

```bash
# Run tests
php run-tests.php

# Run benchmarks
cd tests/
php benchmark.php --duration=60 --concurrency=100
```

## ⚡ Local Benchmark Harness

Use the built-in benchmark harness to measure throughput and latency after each runtime change.

```bash
cd https
make
./bench/run_benchmark.sh
```

Environment overrides:

```bash
BENCH_TARGET_PATH=/json BENCH_DURATION=30 BENCH_CONCURRENCY=300 ./bench/run_benchmark.sh
```

Notes:
- The runner auto-detects `wrk` first, then falls back to `ab`.
- Install `wrk` on macOS with `brew install wrk`.
- Reports are saved under `bench/results/`.

Run endpoint comparison (`/plaintext`, `/json`, `/file`) with one command:

```bash
./bench/run_benchmark_matrix.sh
```

Or via Makefile shortcuts:

```bash
make bench
make bench-matrix
make bench-delta
make bench-stable
make bench-stable-delta
make perf-gate
make perf-gate-quick
make perf-gate-release
```

Lifecycle validation tasks:

```bash
make test-lifecycle
make task-updation
make task-dev
make task-ci
make task-updation-release
```

Workflow guidance:

- `make task-updation`: build + lifecycle regression suite (fastest quality check)
- `make task-dev`: build + lifecycle regression + advisory quick perf gate
- `make task-ci`: build + lifecycle regression + strict perf gate
- `make task-updation-release`: build + lifecycle regression + release perf gate profile

## CI Jobs

- `build` job: runs extension build and standard test flow across PHP version matrix.
- `ecosystem` job: runs strict ecosystem validation (`make task-ci`) with lifecycle checks and strict performance gate.
- CI workflow file: `.github/workflows/ci.yml`.

Update checklist:

```bash
cat UPDATION_LIST.md
```

Quick example:

```bash
BENCH_CONCURRENCY=200 BENCH_DURATION=15 ./bench/run_benchmark_matrix.sh
```

Generate before/after delta from the latest two matrix runs:

```bash
./bench/compare_benchmarks.sh
```

Run deterministic stable profiling with warmup + repeats + median aggregation:

```bash
BENCH_PROFILE=release BENCH_DURATION=20 BENCH_CONCURRENCY=100 BENCH_THREADS=4 BENCH_REPEATS=3 BENCH_WARMUP_SECONDS=5 ./bench/run_benchmark_stable.sh
```

Optional endpoints override:

```bash
BENCH_ENDPOINTS="/plaintext /json /file" ./bench/run_benchmark_stable.sh
```

Generate delta from latest two stable median reports for a profile:

```bash
PERF_PROFILE=release ./bench/compare_stable_benchmarks.sh
```

Run a performance gate (stable benchmark + stable delta + threshold checks):

```bash
make perf-gate
make perf-gate-quick
make perf-gate-release
```

Notes:
- `perf-gate-quick` is advisory (soft-fail) for fast local feedback.
- `perf-gate` and `perf-gate-release` are strict and fail on threshold breaches.
- Gate targets auto-run stable benchmark twice (baseline + candidate) before diffing.

Tune gate thresholds (defaults shown):

```bash
PERF_GATE_MAX_REQ_REGRESSION_PCT=5 \
PERF_GATE_MAX_P95_REGRESSION_PCT=20 \
PERF_GATE_MAX_P99_REGRESSION_PCT=30 \
make perf-gate
```

Exclude noisy endpoints when needed (space-separated):

```bash
PERF_GATE_EXCLUDE_ENDPOINTS="/file" make perf-gate-quick
```

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](.github/CONTRIBUTING.md).

## 📄 License

Licensed under the [Apache License 2.0](LICENSE).

## 🆘 Support

- 📖 [Documentation](docs.md)
- 🌐 Full detailed docs: [Skelves Documentation Site](https://skelves.com/docs)
- 🧪 Local docs view: `http://localhost:5180/docs`
- 🐛 [Issues](https://github.com/KislayPHP/core/issues)
- 💬 [Discussions](https://github.com/KislayPHP/core/discussions)

## 🙏 Acknowledgments

- **embedded HTTP server**: Embedded HTTP server library
- **PHP**: Zend API for extension development
- **TLS library**: SSL/TLS encryption support

---

**Built with ❤️ for the PHP community**
