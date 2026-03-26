# Kislay Core

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Release](https://img.shields.io/badge/Release-0.0.8-orange.svg)]()

Kislay Core is the HTTP runtime for the KislayPHP ecosystem. It provides the embedded HTTP/HTTPS server, strict segment router, request/response lifecycle, middleware, async bridge, and Promise primitives used by the higher-level modules.

## Install

```bash
pie install kislayphp/core:0.0.8
```

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

## Runtime contract

- Routes support only static segments and `:param` segments.
- Regex-style routes and wildcard route fragments are rejected at registration time.
- Middleware uses `function ($req, $res)` and must return a truthy value to continue.
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

Validated locally for `0.0.8` on the current NTS reference machine with tracing, request-id generation, and request logging disabled:

- `/plaintext`: `23789.89 req/s` (`ab -n 100000 -c 100`)
- `/users/:id`: `18915.87 req/s` (`ab -n 40000 -c 100`)
- `/submit/:id`: `12974.19 req/s` (`ab -n 20000 -c 50`)
- RSS remained flat across the sustained `/plaintext` stress run.

Native C++-only paths remain faster than PHP-routed paths. If you need materially higher PHP-route throughput than this NTS single-lane model provides, the next step is ZTS multi-runtime scaling rather than loosening Zend safety.

## Production notes

- Use `Discovery` for service resolution.
- Use `Gateway` for edge routing and rate limiting.
- Use `Persistence` for request-scoped transaction/runtime cleanup.
- Keep `request_id`, `trace`, and `log` off in benchmark profiles unless you are measuring those features specifically.

## Tests

```bash
php run-tests.php
```

Current local release-candidate result for `0.0.8`:

- `15 passed`
- `2 skipped` (`ZTS`-only async coverage)
- `0 failed`

## Support

- Docs: [https://skelves.com/kislayphp/docs/core](https://skelves.com/kislayphp/docs/core)
- Release matrix: [https://skelves.com/kislayphp/docs/release-matrix](https://skelves.com/kislayphp/docs/release-matrix)
- Issues: [https://github.com/KislayPHP/core/issues](https://github.com/KislayPHP/core/issues)
