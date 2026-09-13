# KislayPHP Documentation

**High-performance PHP microservices framework powered by C++ extensions.**

KislayPHP brings Spring Boot-inspired architecture to PHP — a suite of nine C++ extensions (core, gateway, socket, eventbus, persistence, discovery, queue, metrics, config) that deliver non-blocking HTTP, WebSocket, service discovery, message queues, metrics, and more, all accessible from idiomatic PHP code.

---

## Why KislayPHP?

| Capability | Traditional PHP | KislayPHP |
|---|---|---|
| HTTP server | Apache / Nginx + FPM | Built-in async C++ HTTP server |
| WebSocket | External lib / Ratchet | Native EventBus (Socket.IO) |
| Service discovery | None / manual | Built-in Discovery extension |
| Circuit breaker | None | API Gateway with CB threshold |
| Metrics | External APM | Prometheus-compatible Metrics ext |
| Database | PDO / ORM | Persistence extension |
| Scheduler | Cron + shell | $app->schedule() / every() |
| Configuration | .env only | Config ext (layered global/env/project/service/node) |

---

## Extensions at a Glance

| Extension | Install (via [PIE](https://github.com/php/pie), compiled from source) | Purpose |
|---|---|---|
| Core | `pie install kislayphp/core` | HTTP server, routing, middleware, HTTPS |
| Gateway | `pie install kislayphp/gateway` | Reverse proxy, load balancing, circuit breaker |
| Discovery | `pie install kislayphp/discovery` | Service register, resolve, weighted LB |
| EventBus | `pie install kislayphp/eventbus` | WebSocket / Socket.IO, namespaces, auth |
| Queue | `pie install kislayphp/queue` | Enqueue, TTL, DLQ, priority, delayed |
| Metrics | `pie install kislayphp/metrics` | Counter / gauge / histogram, Prometheus export |
| Persistence | `pie install kislayphp/persistence` | DB connect, raw SQL + bindings, migrations |
| Config | `pie install kislayphp/config` | Typed getters, layered scopes, standalone config server |

See [Installation](getting-started/installation.md) — there are no pre-built binaries yet, so this compiles the extension on your machine (C++ compiler + headers required).

---

## Quick Example

```php
<?php
$app = new Kislay\Core\App();

$app->get('/hello/:name', function ($req, $res) {
    $res->json(['message' => 'Hello, ' . $req->params['name']]);
});

$app->listen('0.0.0.0', 8080);
```

Run it:

```bash
php server.php
# Listening on http://0.0.0.0:8080
```

---

## Next Steps

- [Installation](getting-started/installation.md) — get all extensions installed
- [Quick Start](getting-started/quick-start.md) — routes, middleware, JSON responses
- [Architecture](getting-started/architecture.md) — why a real deployment is more than one process, and why `core`/`gateway`/`socket` must never share one
- [Spring Boot Mapping](getting-started/spring-boot-mapping.md) — familiar Spring concepts mapped to PHP
- [Performance & Benchmarks](advanced/performance.md) — v0.0.7 benchmark results vs Fastify, Go, Spring Boot

---

## Performance Highlights (v0.0.7)

| Scenario | KislayPHP | vs Fastify | vs Go |
|---|---|---|---|
| Route matching | **17,496 req/s** | +2.4× | +16× |
| JSON (small) | **17,918 req/s** | +1.7× | +3.6× |
| JSON (100 KB) | **13,134 req/s** | +1.7× | +1.1× |
| Plaintext | **16,375 req/s** | +1.0× | — |

All measurements: 10,000 req · c=100 · localhost · Apple Silicon · PHP 8.5.2 NTS.
→ See [full benchmark report](advanced/performance.md).
