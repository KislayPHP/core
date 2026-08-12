# Kislay Core

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Release](https://img.shields.io/badge/Release-1.0.0-orange.svg)]()

Kislay Core is the HTTP runtime for the KislayPHP ecosystem. It provides the embedded HTTP/HTTPS server, strict segment router, request/response lifecycle, middleware, async bridge, and Promise primitives used by the higher-level modules.

## Install

Prerequisites for PIE/source builds:

- macOS (Homebrew): `brew install libuv`
- Debian/Ubuntu: install the development packages for `libuv`, `curl`, and OpenSSL

Ubuntu 24.04 reference flow used for the release verification:

```bash
sudo apt-get update
sudo apt-get install -y pkg-config libcurl4-openssl-dev libssl-dev libuv1-dev
```

```bash
pie install kislayphp/core:1.0.0
```

Automation note:

- in a non-interactive automation session on macOS, PIE may stop after the build step because the final copy still goes through `sudo`
- the built module can still be validated directly from the PIE working directory before the final interactive install step

```ini
extension=kislayphp_extension.so
```

Build from source:

```bash
git clone https://github.com/KislayPHP/core.git
cd core
phpize
./configure --enable-kislayphp_extension
make -j4
sudo make install
```

## New in 1.0.0

- **`Kislay\Core\AttributeRouter`** — PHP 8 attribute-based routing (`#[Route]`, `#[Get]`, `#[Post]`, etc.) for declaring routes on controller methods instead of imperative `$app->get(...)` calls.
- **`Kislay\Core\EventPublisher`** — a typed, in-process event bus (Spring-style `ApplicationEventPublisher`) with class-hierarchy and interface-based listener dispatch.
- Hot-path work: flat vector-backed HTTP header storage (`FlatHeaders`) cutting per-request header allocations from N to 1, an atomic request-completion flag with a short spin-wait so fast handlers avoid extra context switches, and thread-local CURL handle pooling + a non-blocking retry queue for the async HTTP client.
- **Fixed:** `/actuator/health` could crash the whole process under concurrent load — its health-indicator invocation loop called Zend APIs directly from a raw civetweb worker thread instead of the safe single-dedicated-PHP-thread request pool, which on NTS builds could corrupt Zend's shared heap under concurrent access. Now serialized behind a mutex.

## Runtime contract

- Routes support only static segments and `:param` segments.
- Regex-style routes and wildcard route fragments are rejected at registration time.
- Middleware supports two signatures: `function ($req, $res)`, which must return a truthy value to continue (returning falsy/nothing halts the chain with a 403 unless the middleware already wrote its own response), or `function ($req, $res, $next)` (fixed 2026-08-12 — previously threw `ArgumentCountError` on every request), where calling `$next()` continues the chain and not calling it halts it. **`$next()` is synchronous-only, not a full "onion model" continuation**: it must be called from within the middleware's own function body (a later/deferred call, e.g. from inside a promise callback, won't work), and code written *after* the `$next()` call runs before the rest of the chain executes, not after it returns — unlike Express.js, where code after `next()` runs once the downstream chain has fully unwound. If you need to run cleanup code after the whole chain (including the route handler) completes, use `onRequestEnd()` instead.
- Query/body parsing is lazy.
- `listenAsync()` requires ZTS. On NTS, run `listen()` in its own process.
- `AsyncHttp` self-requests are rejected in single PHP runtime mode to avoid deadlocks.

## Quick start

```php
<?php

$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('request_id', false);
$app->setOption('trace', false);

$app->use('/api', function ($req, $res) {
    $res->set('X-Powered-By', 'Kislay');
    return true;
});

$app->get('/api/users/:id', function ($req, $res) {
    $res->json([
        'id' => $req->param('id'),
        'search' => $req->query('q', ''),
    ], 200);
});

$app->post('/api/users/:id', function ($req, $res) {
    $res->json([
        'id' => $req->param('id'),
        'email' => $req->input('email'),
    ], 200);
});

$app->listen('0.0.0.0', 8080);
```

## Request API

```php
$req->param('id');
$req->query('name');
$req->input('email');
$req->getJson();
$req->header('authorization');
```

## Async primitives

```php
$app->setOption('async', true);
$app->setOption('async_threads', 4);

async(function () {
    return heavy_computation();
})->then(function ($result) {
    echo $result;
});

$http = new Kislay\Core\AsyncHttp();
$http->get('https://api.example.com/data');
$http->retry(2, 200);
$http->executeAsync()->then(function () use ($http) {
    echo $http->getResponseCode();
});
```

## Performance notes

Validated locally on the NTS reference machine with tracing, request-id generation, and request logging disabled (figures below are from the `0.0.10` validation run; the hot-path work landing in `1.0.0` has not been re-measured against this exact `ab` command yet):

- `/plaintext`: `23789.89 req/s` (`ab -n 100000 -c 100`)
- `/users/:id`: `18915.87 req/s` (`ab -n 40000 -c 100`)
- `/submit/:id`: `12974.19 req/s` (`ab -n 20000 -c 50`)
- RSS remained flat across the sustained `/plaintext` stress run.

Native C++-only paths remain faster than PHP-routed paths. If you need materially higher PHP-route throughput than this NTS single-lane model provides, the next step is ZTS multi-runtime scaling rather than loosening Zend safety.

### Cross-language comparison (1.0.0)

`GET /plaintext`, wrk (2 threads, 20 connections, 3s + 5s warmup), 10-core reference machine, each server using all available cores where supported. Produced by `compare/run_compare.sh`:

| Framework | req/s | p50 | p99 |
|---|---:|---:|---:|
| Go (net/http) | 184,757 | 77µs | 368µs |
| Node.js (native, cluster) | 182,334 | 74µs | 5.69ms |
| Spring Boot (WebFlux + Netty) | 174,963 | 78µs | 2.09ms |
| Node.js (native, single process) | 135,177 | 135µs | 312µs |
| Node.js (Fastify) | 121,723 | 153µs | 342µs |
| **KislayPHP Core** | 104,641 | 83µs | **150µs** |

KislayPHP Core trails on peak throughput here — it's bound to a single dedicated PHP execution thread on NTS builds regardless of civetweb's I/O thread count (see the ZTS note above) — but its p99 is the tightest of the group, well under Go's and far under Node cluster's and Spring's multi-millisecond tail. Reproduce with `../compare/run_compare.sh` from the repo root, or the quick `../perf_smoke_test.sh` for a faster (and less statistically rigorous) sanity check.

## Production notes

- Use `Discovery` for service resolution.
- Use `Gateway` for edge routing and rate limiting.
- Use `Persistence` for request-scoped transaction/runtime cleanup.
- Keep `request_id`, `trace`, and `log` off in benchmark profiles unless you are measuring those features specifically.
- **Do not load `core`, `gateway`, and `socket` together in the same PHP process.** All three vendor their own copy of civetweb (embedded multi-threaded HTTP server) and export non-static symbols like `mg_start`. On platforms that link PHP extensions with `-flat_namespace` (notably macOS), loading two or more of these extensions into one process risks one's compiled civetweb code silently shadowing another's — with no error, no warning, just undefined behavior up to and including crashes. Run each in its own process (e.g. `core` for your HTTP app, `socket` for a separate WebSocket process, fronted by `gateway`) rather than combining `-d extension=` flags for more than one of them.

## Tests

```bash
php run-tests.php
```

Current local result (`1.0.0`):

- `15 passed`
- `2 skipped` (`ZTS`-only async coverage)
- `0 failed`

Clean Docker verification on PHP 8.5 RC also passed before release:

- `NTS /plaintext`: `11157.65 req/s`, `0 failed`, `p95 30 ms`
- `NTS /json`: `20085.16 req/s`, `0 failed`, `p95 9 ms`
- `ZTS /plaintext`: `16697.28 req/s`, `0 failed`, `p95 13 ms`
- `ZTS /json`: `25055.87 req/s`, `0 failed`, `p95 6 ms`

## Support

- Docs: [https://skelves.com/kislayphp/docs/core](https://skelves.com/kislayphp/docs/core)
- Release matrix: [https://skelves.com/kislayphp/docs/release-matrix](https://skelves.com/kislayphp/docs/release-matrix)
- Issues: [https://github.com/KislayPHP/core/issues](https://github.com/KislayPHP/core/issues)
