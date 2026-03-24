# KislayPHP Documentation

**High-performance PHP microservices framework powered by C++ extensions.**

KislayPHP brings Spring Boot-inspired architecture to PHP — a suite of eight C++ extensions that deliver non-blocking HTTP, WebSocket, service discovery, message queues, metrics, and more, all accessible from idiomatic PHP code.

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
| Configuration | .env only | Config ext (env / consul / etcd) |

---

## Extensions at a Glance

| Extension | Composer Package | Purpose |
|---|---|---|
| Core | `composer require kislayphp/core` | HTTP server, routing, middleware, HTTPS |
| Gateway | `composer require kislayphp/gateway` | Reverse proxy, load balancing, circuit breaker |
| Discovery | `composer require kislayphp/discovery` | Service register, resolve, weighted LB |
| EventBus | `composer require kislayphp/eventbus` | WebSocket / Socket.IO, namespaces, auth |
| Queue | `composer require kislayphp/queue` | Enqueue, TTL, DLQ, priority, delayed |
| Metrics | `composer require kislayphp/metrics` | Counter / gauge / histogram, Prometheus export |
| Persistence | `composer require kislayphp/persistence` | DB connect, CRUD, migrations |
| Config | `composer require kislayphp/config` | Typed getters, live refresh, consul/etcd |

---

## Quick Example

```php
<?php
$app = new Kislay\App();
$app->setOption('port', 8080);

$app->get('/hello/{name}', function ($req, $res) {
    $res->json(['message' => 'Hello, ' . $req->params['name']]);
});

$app->listen();
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
