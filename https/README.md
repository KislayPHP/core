# KislayCore

> High-performance C++ PHP extension providing an embedded HTTP/HTTPS server with routing, middleware, and async primitives for building lightweight microservices and APIs.

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Performance](https://img.shields.io/badge/Performance-63K%20req%2Fsec-orange.svg)]()

## Installation

**Via PIE (recommended):**
```bash
pie install kislayphp/core
```

Add to `php.ini`:
```ini
extension=kislayphp_extension.so
```

**Build from source:**
```bash
git clone https://github.com/KislayPHP/core.git
cd core && phpize && ./configure --enable-kislayphp_extension && make && sudo make install
```

## Requirements

- PHP 8.2+
- PHP ZTS build recommended for `threads > 1` (NTS builds auto-clamp to 1 thread)
- OpenSSL for HTTPS/TLS support

## Quick Start

```php
<?php
$app = new Kislay\Core\App();

$app->setOption('threads', 4);

$app->use(function ($req, $res, $next) {
    $res->header('X-Powered-By', 'KislayPHP');
    $next();
});

$app->get('/api/users', function ($req, $res) {
    $res->json([['id' => 1, 'name' => 'Alice']]);
});

$app->post('/api/users', function ($req, $res) {
    $body = $req->body();
    $res->status(201)->json(['id' => 2, 'name' => $body['name'] ?? 'Unknown']);
});

$app->listen('0.0.0.0', 8080);
```

## API Reference

### `App`

#### `listen(string $host, int $port): bool`
Starts the HTTP server and blocks until stopped.
- `$host` — bind address, e.g. `'0.0.0.0'`
- `$port` — TCP port, e.g. `8080`
- Returns `true` on clean shutdown

#### `listenAsync(string $host, int $port): bool`
Starts the server in the background without blocking. Use `wait()` to block later.
```php
$app->listenAsync('0.0.0.0', 8080);
// perform startup tasks here
$app->wait();
```

#### `wait(?int $timeoutMs = null): bool`
Blocks until the server stops, or until `$timeoutMs` milliseconds elapse.

#### `isRunning(): bool`
Returns `true` if the server is currently accepting connections.

#### `stop(): bool`
Gracefully stops a running server.

#### `setOption(string $key, mixed $value): bool`
Sets a server option before calling `listen()`. See [Configuration](#configuration).

#### `use(callable $middleware): void`
Registers global middleware. Signature: `function($req, $res, $next): void`.

#### `use(string $path, callable $middleware): void`
Registers path-scoped middleware. Runs only when the request path starts with `$path`.

#### `get/post/put/delete/patch/head(string $path, callable $handler): void`
Registers a route handler for the given HTTP method and path.
```php
$app->get('/users/:id', function ($req, $res) {
    $id = $req->param('id');
    $res->json(['id' => $id]);
});
```

#### `route(string $method, string $path, callable $handler): void`
Generic route registration; equivalent to the HTTP-method-specific helpers.

#### `all(string $path, callable $handler): void`
Matches any HTTP method on the given path.

#### `onNotFound(callable $handler): void`
Registers a 404 fallback handler. Signature: `function($req, $res): void`.

#### `onError(callable $handler): void`
Registers a global error handler. Signature: `function($err, $req, $res, $next): void`.
Four-parameter handlers are auto-detected as error middleware.

#### `onRequestStart(callable $hook): void`
Hook called at the very start of every request lifecycle, before middleware runs.

#### `onRequestEnd(callable $hook): void`
Hook called after the response is sent. Use for cleanup (DB rollback, metric flush).

---

### `Request`

| Method | Signature | Description |
|--------|-----------|-------------|
| `method` | `method(): string` | HTTP verb in upper-case (`GET`, `POST`, …) |
| `uri` | `uri(): string` | Full request URI including query string |
| `body` | `body(): mixed` | Parsed request body (JSON decoded if `Content-Type: application/json`) |
| `headers` | `headers(): array` | All request headers as a key-value array |
| `header` | `header(string $name): ?string` | Single header value; `null` if absent |
| `ip` | `ip(): string` | Client IP; respects `trust_proxy` / `trusted_proxies` option |
| `id` | `id(): string` | Auto-generated request correlation ID |
| `query` | `query(?string $key = null): mixed` | Query string params; pass key for a single value |
| `param` | `param(string $name): ?string` | Named route parameter (e.g. `:id`) |

---

### `Response`

| Method | Signature | Description |
|--------|-----------|-------------|
| `json` | `json(mixed $data, int $status = 200): void` | Send a JSON response |
| `send` | `send(string $body, int $status = 200): void` | Send plain text / raw response |
| `html` | `html(string $html, int $status = 200): void` | Send an HTML response |
| `status` | `status(int $code): static` | Set HTTP status code (chainable) |
| `header` | `header(string $name, string $value): static` | Set a response header (chainable) |
| `redirect` | `redirect(string $url, int $code = 302): void` | Send a redirect response |
| `end` | `end(): void` | Finalize and send response with no body |
| `sendStatus` | `sendStatus(int $code): void` | Send status code with standard status-text body |

---

### `async()` and `AsyncHttp`

```php
// Run heavy work off the request thread
async(function () {
    return expensive_computation();
})->then(function ($result) {
    echo "Done: $result\n";
})->catch(function ($err) {
    echo "Error: " . $err->getMessage() . "\n";
});

// Non-blocking outbound HTTP with automatic retries
$http = new Kislay\Core\AsyncHttp();
$http->get('https://api.example.com/data')
     ->retry(3, 500);    // 3 retries, 500 ms back-off
$http->executeAsync()->then(function () use ($http) {
    echo $http->getResponseCode();
});
```

## Configuration

### `setOption()` keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `threads` | `int` | `1` | HTTP IO worker threads (clamped to 1 on NTS PHP) |
| `async_threads` | `int` | `4` | Background task worker pool size |
| `log` | `bool` | `false` | Enable structured request logging |
| `error_log` | `bool` | `true` | Enable error logging |
| `trust_proxy` | `bool` | `false` | Trust `X-Forwarded-For` header for client IP |
| `trusted_proxies` | `string` | `''` | Comma-separated list of trusted proxy CIDRs |
| `timeout_ms` | `int` | `10000` | Read timeout in milliseconds |
| `tls_cert` | `string` | `''` | Path to TLS certificate PEM file |
| `tls_key` | `string` | `''` | Path to TLS private key PEM file |
| `cors` | `bool` | `false` | Enable CORS headers |
| `referrer_policy` | `string` | `strict-origin-when-cross-origin` | `Referrer-Policy` header value; `'off'` to disable |
| `document_root` | `string` | `''` | Static file document root |
| `max_body` | `int` | `1048576` | Maximum request body size in bytes |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `KISLAYPHP_HTTP_THREADS` | Number of HTTP IO threads |
| `KISLAYPHP_ASYNC_THREADS` | Number of background worker threads |
| `KISLAYPHP_HTTP_READ_TIMEOUT_MS` | Read timeout in milliseconds |
| `KISLAYPHP_HTTP_MAX_BODY` | Max request body in bytes |
| `KISLAYPHP_HTTP_LOG` | `1` to enable request logging |
| `KISLAYPHP_HTTP_TLS_CERT` | Path to TLS certificate |
| `KISLAYPHP_HTTP_TLS_KEY` | Path to TLS private key |

### `php.ini` Settings

```ini
kislayphp.http.threads = 4
kislayphp.http.async_threads = 4
kislayphp.http.read_timeout_ms = 10000
kislayphp.http.max_body = 1048576
kislayphp.http.log = 1
kislayphp.http.tls_cert = "/path/to/cert.pem"
kislayphp.http.tls_key  = "/path/to/key.pem"
```

## Events

KislayCore exposes lifecycle hooks rather than named events:

| Hook | When | Typical Use |
|------|------|-------------|
| `onRequestStart` | Before middleware/route | Attach DB transaction, init request context |
| `onRequestEnd` | After response sent | Rollback uncommitted transactions, flush metrics |

```php
// Auto-registers both hooks for transaction safety
Kislay\Persistence\Runtime::attach($app);
```

## Examples

### HTTPS Server

```php
$app = new Kislay\Core\App();
$app->setOption('tls_cert', '/etc/ssl/cert.pem');
$app->setOption('tls_key',  '/etc/ssl/key.pem');
$app->get('/', fn($req, $res) => $res->send('Secure!'));
$app->listen('0.0.0.0', 8443);
```

### Error Middleware

```php
$app->onError(function ($err, $req, $res, $next) {
    $res->status(500)->json(['error' => $err->getMessage()]);
});
```

### Non-Blocking Start

```php
$app->listenAsync('0.0.0.0', 8080);
// warm up caches, run migrations, etc.
if ($app->isRunning()) {
    echo "Server ready\n";
    $app->wait();
}
```

### Path-Scoped Middleware

```php
$app->use('/api', function ($req, $res, $next) {
    $token = $req->header('Authorization');
    if (!$token) {
        $res->status(401)->json(['error' => 'Unauthorized']);
        return;
    }
    $next();
});
```

## Related Extensions

| Extension | Use Case |
|-----------|----------|
| [kislayphp/gateway](https://github.com/KislayPHP/gateway) | Reverse proxy / API gateway routing traffic to Core services |
| [kislayphp/discovery](https://github.com/KislayPHP/discovery) | Service registration and health-aware load balancing |
| [kislayphp/persistence](https://github.com/KislayPHP/persistence) | Request-scoped DB transactions via `Runtime::attach($app)` |
| [kislayphp/eventbus](https://github.com/KislayPHP/eventbus) | Realtime WebSocket / event fanout alongside HTTP |
| [kislayphp/metrics](https://github.com/KislayPHP/metrics) | In-process counters and gauges with Prometheus export |

## License

Licensed under the [Apache License 2.0](LICENSE).
