# API Gateway

## Overview

`kislayphp/gateway` turns any KislayPHP application into a programmable reverse proxy. It provides route-level proxying to upstream services, weighted round-robin load balancing, a configurable circuit breaker, and JWT-based authentication guards — all wired together without an external API gateway process.

## Installation

```bash
composer require kislayphp/gateway
```

GitHub: <https://github.com/KislayPHP/php-kislay-gateway>

---

## Key Features

- **Reverse proxy** — forward requests to any HTTP upstream by URL pattern
- **Load balancing** — round-robin and weighted round-robin across multiple upstreams
- **Circuit breaker** — open after N consecutive failures; auto-closes after reset window
- **`requireAuth`** — attach JWT validation to any proxied route
- **Header manipulation** — add, remove, or rewrite request/response headers before forwarding
- **Retry logic** — configurable retry count and backoff on 5xx upstream errors
- **Timeout per route** — override the global request timeout for slow upstreams

---

## Quick Example

```php
<?php
$app     = new Kislay\App();
$gateway = new Kislay\Gateway($app);

// Simple proxy — all /api/users/* → user-service
$gateway->addRoute('/api/users', 'http://user-service:3000', [
    'strip_prefix' => true,
]);

// Load-balanced route with circuit breaker
$gateway->addRoute('/api/orders', [
    'http://order-service-1:3001',
    'http://order-service-2:3001',
], [
    'lb_strategy'  => 'round_robin',
    'cb_threshold' => 5,    // open after 5 consecutive failures
    'cb_timeout'   => 30,   // seconds before half-open retry
    'retries'      => 2,
]);

// Authenticated route
$gateway->addRoute('/api/admin', 'http://admin-service:4000', [
    'requireAuth' => true,
    'timeout'     => 10,
]);

$app->listen();
```

---

## Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| `strip_prefix` | bool | `false` | Remove matched prefix before forwarding |
| `lb_strategy` | string | `round_robin` | `round_robin` or `weighted` |
| `cb_threshold` | int | `0` (disabled) | Failures before circuit opens |
| `cb_timeout` | int | `60` | Seconds in open state before retry |
| `retries` | int | `0` | Retry count on 5xx |
| `timeout` | int | `30` | Per-route upstream timeout (s) |
| `requireAuth` | bool | `false` | Enforce JWT on this route |
| `add_headers` | array | `[]` | Headers to inject into upstream request |
