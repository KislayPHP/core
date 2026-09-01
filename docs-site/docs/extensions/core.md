# Core HTTP Server

## Overview

`kislayphp/core` is the foundation of every KislayPHP application. It embeds a high-performance, multi-threaded C++ HTTP/1.1 server directly into the PHP process — no Apache, Nginx, or PHP-FPM required. The extension exposes a fluent routing API, middleware chains, HTTPS termination, and a rich request/response object model.

The default, stable backend is [CivetWeb](https://github.com/civetweb/civetweb); an experimental libuv-based backend exists for higher concurrency but is not the default and has known open issues on macOS — see [core's CLAUDE.md](https://github.com/KislayPHP/core/blob/main/CLAUDE.md) before opting into it.

## Installation

There are no pre-built binaries yet — this compiles from source via [PIE](https://github.com/php/pie) or a manual `phpize` build. See [Installation](../getting-started/installation.md) for the real steps and prerequisites.

GitHub: <https://github.com/KislayPHP/core>

---

## Key Features

- **Multi-threaded C++ HTTP core** — CivetWeb-based by default
- **Express-style routing** — `get / post / put / delete / patch`
- **Middleware chains** — 3-param `($req, $res, $next)` and 4-param error handlers
- **Route parameters** — `:name`-style segments mapped to `$req->params` (only static and `:param` segments are supported; regex/wildcard routes are rejected at registration time)
- **HTTPS / TLS** — pass a TLS options array to `listen()`, or set `tls_cert`/`tls_key` via `setOption()`
- **Response helpers** — `json()`, `send()`, `status()`, `header()`, `redirect()`
- **Built-in Actuator** — `/actuator/health`, `/actuator/metrics`, `/actuator/routes` (opt-in via `setOption('actuator', true)`)

---

## Quick Example

```php
<?php
$app = new Kislay\Core\App();

$app->setOption('workers', 4); // thread count

// Global middleware
$app->use(function ($req, $res, $next) {
    $res->header('X-Powered-By', 'KislayPHP');
    $next();
});

// Routes
$app->get('/', function ($req, $res) {
    $res->json(['status' => 'ok']);
});

$app->get('/users/:id', function ($req, $res) {
    $res->json(['userId' => $req->params['id']]);
});

$app->post('/users', function ($req, $res) {
    $data = $req->body;
    $res->status(201)->json(['created' => true, 'data' => $data]);
});

// Host and port are arguments to listen(), not setOption() keys
$app->listen('0.0.0.0', 8080);

// HTTPS variant: pass a TLS options array as listen()'s 3rd argument,
// or set setOption('tls_cert', ...) / setOption('tls_key', ...) beforehand.
```

---

## Configuration

Set via `$app->setOption($key, $value)` before calling `listen()`. `host` and `port` are **not** options — they're `listen()`'s required first two arguments. Any unrecognized key triggers an `E_WARNING` and is otherwise ignored.

| Option | Type | Default | Description |
|---|---|---|---|
| `workers` (or `worker_count`) | int | `1` (`kislayphp.http.threads` ini) | Worker thread count |
| `tls_cert` | string | — | Path to TLS certificate (must be a readable file) |
| `tls_key` | string | — | Path to TLS private key (must be a readable file) |
| `actuator` (or `actuator_enabled`) | bool | `false` | Enable actuator endpoints |
| `request_timeout_ms` (or `read_timeout_ms`) | int | `10000` (`kislayphp.http.read_timeout_ms` ini) | Milliseconds before a request times out |
| `max_body_bytes` (or `max_request_size`, `max_body`) | int | `0` = unbounded (`kislayphp.http.max_body` ini) | Max request body bytes |
| `cors` (or `cors_enabled`) | bool | `false` (`kislayphp.http.cors` ini) | Enable permissive CORS headers |
