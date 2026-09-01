# API Gateway

## Overview

`kislayphp/gateway` turns any KislayPHP application into a programmable reverse proxy. It provides route-level proxying to upstream services, weighted round-robin load balancing, a configurable circuit breaker, and JWT-based authentication guards — all wired together without an external API gateway process.

## Installation

There are no pre-built binaries yet — this compiles from source via [PIE](https://github.com/php/pie) or a manual `phpize` build, the same as core. See [Installation](../getting-started/installation.md).

GitHub: <https://github.com/KislayPHP/gateway>

---

## Key Features

- **Reverse proxy** — forward requests to any HTTP upstream, route by exact method+path
- **Load balancing** — round-robin across multiple targets via `registerService()`
- **Circuit breaker** — open after N consecutive failures per upstream host; process-wide, configured via env vars, not per-route
- **`requireAuth`** — JWT validation, enabled gateway-wide (not per individual route)
- **Standalone process** — `Gateway` does not wrap a Core `App`; it never runs your application code, only proxies to it (see [Architecture](../getting-started/architecture.md))

---

## Quick Example

```php
<?php
$gateway = new Kislay\Gateway\Gateway();

// Simple proxy — GET /api/users → user-service (method, path, target: all required strings)
$gateway->addRoute('GET', '/api/users', 'http://user-service:3000');

// Load-balanced pool — round-robins across all targets
$gateway->registerService('order-service', [
    'http://order-service-1:3001',
    'http://order-service-2:3001',
]);
$gateway->addServiceRoute('GET', '/api/orders', 'order-service');

// JWT auth, gateway-wide — exclude specific paths from enforcement
$gateway->requireAuth(getenv('JWT_SECRET'));
$gateway->setAuthExclude(['/health', '/api/users']);

$gateway->listen('0.0.0.0', 8080);
```

---

## Configuration

Verified route/service methods (all on the `Gateway` object):

| Method | Signature | Description |
|---|---|---|
| `addRoute` | `(string $method, string $path, string $target)` | One static upstream per route |
| `registerService` | `(string $name, string[] $targets)` | Named round-robin pool |
| `addServiceRoute` | `(string $method, string $path, string $service)` | Route to a registered pool |
| `requireAuth` | `(string $secret, ?array $options = null)` | Enable JWT validation gateway-wide |
| `setAuthExclude` | `(string[] $paths)` | Paths exempt from JWT |
| `setThreads` | `(int $count)` | Worker thread count, before `listen()` |
| `setFallbackTarget` / `setFallbackService` | `(string $target)` / `(string $service)` | Catch-all for unmatched routes |
| `listen` | `(string $host, int $port)` | Start serving — no TLS 3rd argument here (unlike Core's `App::listen()`) |

Circuit breaker and rate limiting are process-wide, not per-route, and configured via environment variables (not `setOption`/method calls):

| Env Var | Default | Description |
|---|---|---|
| `KISLAY_GATEWAY_CIRCUIT_BREAKER_ENABLED` | `false` | Enable the circuit breaker |
| `KISLAY_GATEWAY_CB_FAILURE_THRESHOLD` | `5` | Consecutive failures before a host's circuit opens |
| `KISLAY_GATEWAY_CB_OPEN_SECONDS` | `30` | Seconds a circuit stays open before a retry |
| `KISLAY_GATEWAY_THREADS` | `1` | CivetWeb worker threads (see `setThreads()` above for the per-instance equivalent) |
