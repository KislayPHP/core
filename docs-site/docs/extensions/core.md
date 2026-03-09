# Core HTTP Server

## Overview

`kislayphp/core` is the foundation of every KislayPHP application. It embeds a high-performance, non-blocking C++ HTTP/1.1 and HTTP/2 server directly into the PHP process — no Apache, Nginx, or PHP-FPM required. The extension exposes a fluent routing API, middleware chains, HTTPS termination, and a rich request/response object model.

## Installation

```bash
composer require kislayphp/core
```

GitHub: <https://github.com/KislayPHP/php-kislay-core>

---

## Key Features

- **Async C++ HTTP core** — libuv-based event loop, no blocking I/O
- **Express-style routing** — `get / post / put / delete / patch / options`
- **Middleware chains** — 3-param `($req, $res, $next)` and 4-param error handlers
- **Route parameters** — `{name}` segments mapped to `$req->params`
- **HTTPS / TLS** — pass `cert` and `key` options to enable SSL termination
- **Response helpers** — `json()`, `send()`, `status()`, `header()`, `redirect()`
- **Built-in Actuator** — `/actuator/health`, `/actuator/metrics`, `/actuator/routes`

---

## Quick Example

```php
<?php
$app = new Kislay\App();

$app->setOption('port',    8080);
$app->setOption('host',    '0.0.0.0');
$app->setOption('workers', 4);         // thread count

// Global middleware
$app->use(function ($req, $res, $next) {
    $res->header('X-Powered-By', 'KislayPHP');
    $next();
});

// Routes
$app->get('/', function ($req, $res) {
    $res->json(['status' => 'ok']);
});

$app->get('/users/{id}', function ($req, $res) {
    $res->json(['userId' => $req->params['id']]);
});

$app->post('/users', function ($req, $res) {
    $data = $req->body;
    $res->status(201)->json(['created' => true, 'data' => $data]);
});

// HTTPS variant
// $app->setOption('cert', '/etc/ssl/cert.pem');
// $app->setOption('key',  '/etc/ssl/key.pem');

$app->listen();
```

---

## Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| `port` | int | `8080` | TCP port to bind |
| `host` | string | `0.0.0.0` | Bind address |
| `workers` | int | CPU count | IO thread pool size |
| `cert` | string | — | Path to TLS certificate |
| `key` | string | — | Path to TLS private key |
| `actuator` | bool | `false` | Enable actuator endpoints |
| `request_timeout` | int | `30` | Seconds before request times out |
| `max_body_size` | int | `1048576` | Max request body bytes (1 MB) |
