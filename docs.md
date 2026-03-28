# Kislay Core Documentation

## Overview

`kislayphp/core:0.0.9` is the current HTTP runtime release for KislayPHP. This release line keeps the hybrid async architecture, fixes the Linux request-object lifecycle crash found during clean Docker validation, and keeps the shipped source tree buildable through PIE:

- strict segment router only (`/users/:id`)
- compiled middleware chains per matched route
- lazy query and form parsing
- single-thread-safe Promise resolution on the PHP lane
- guarded async self-request behavior in single-runtime mode

## Installation

Build prerequisites:

- macOS (Homebrew): `brew install libuv llhttp`
- Linux: install the development packages for `libuv` and `llhttp`

```bash
pie install kislayphp/core:0.0.9
```

```ini
extension=kislayphp_extension.so
```

## Route model

Supported:

```php
$app->get('/users/:id', $handler);
$app->post('/orders/:orderId/items/:itemId', $handler);
```

Rejected at registration time:

- regex fragments like `/users/(.*)`
- wildcard-like route fragments
- partial param fragments like `/users/:bad-name`

## Middleware model

Middleware signature:

```php
function ($req, $res) {
    return true;
}
```

Rules:

- return truthy to continue
- return `false` to short-circuit
- no `$next` object is used in the hot path anymore
- compiled path middleware is only applied to matched routes

Example:

```php
$app->use('/api', function ($req, $res) {
    if ($req->path() === '/api/users/blocked') {
        $res->status(401)->send('blocked');
        return false;
    }

    $res->set('X-Scoped', '1');
    return true;
});
```

## Request parsing

Parsing is lazy:

- `query()` parses query string on first access
- `input()` parses form-urlencoded body on first access
- `getJson()` parses JSON body on first access and caches it for the request

Relevant APIs:

```php
$req->param('id');
$req->query('name', '');
$req->input('email');
$req->all();
$req->getJson();
$req->isJson();
```

## Async behavior

`async()` and `AsyncHttp` are still available, but with stricter runtime guards:

- Promise resolution happens only on the PHP drain loop
- worker threads stay pure C++
- `AsyncHttp` self-requests are rejected in single-runtime mode
- `listenAsync()` requires ZTS

For NTS production deployments:

- run `listen()` in a dedicated process
- scale with multiple service processes behind Gateway

## Local validation summary

Release-candidate local results on the current NTS machine:

- `/plaintext`: `23789.89 req/s` at `ab -n 100000 -c 100`
- `/users/:id`: `18915.87 req/s` at `ab -n 40000 -c 100`
- `/submit/:id`: `12974.19 req/s` at `ab -n 20000 -c 50`
- PHPT: `15 passed`, `2 skipped`, `0 failed`
- RSS stayed effectively flat during the sustained exact-route stress run

## Operational guidance

Keep these off in benchmark profiles unless required:

- `request_id`
- `trace`
- `log`

Useful runtime options:

```php
$app->setOption('request_id', false);
$app->setOption('trace', false);
$app->setOption('log', false);
$app->setOption('gc_interval_requests', 1000);
$app->setOption('async_threads', 4);
```

## Release notes for 0.0.9

- construct and destroy request trace strings correctly, fixing the Linux first-request segfault in clean Docker runs
- keep the single-lane fast path for local NTS builds while preserving the guarded async runtime model
- add `bench/docker_benchmark.sh` so NTS and ZTS can be validated from clean PHP 8.5 containers before release
- local PHPT suite and Docker source builds both stay green for this release line
