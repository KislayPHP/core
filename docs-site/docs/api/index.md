# C++ API Reference (Doxygen)

The C++ internals of each extension are documented using Doxygen. The generated HTML is published to GitHub Pages alongside this MkDocs site.

---

## Extension API Docs

| Extension | Doxygen URL | Description |
|---|---|---|
| Core | <https://kislayphp.github.io/php-kislay-core/api/> | HTTP server, router, request/response C++ classes |
| Gateway | <https://kislayphp.github.io/php-kislay-gateway/api/> | Proxy engine, circuit breaker, load balancer |
| Discovery | <https://kislayphp.github.io/php-kislay-discovery/api/> | Registry, resolver, heartbeat manager |
| EventBus | <https://kislayphp.github.io/php-kislay-eventbus/api/> | WebSocket server, Socket.IO protocol layer |
| Queue | <https://kislayphp.github.io/php-kislay-queue/api/> | Queue engine, storage backends, worker pool |
| Metrics | <https://kislayphp.github.io/php-kislay-metrics/api/> | Metric types, registry, Prometheus serialiser |
| Persistence | <https://kislayphp.github.io/php-kislay-persistence/api/> | Connection pool, query builder, migration runner |
| Config | <https://kislayphp.github.io/php-kislay-config/api/> | Config cache, backend adapters, watcher thread |

---

## Generating Locally

Each repository ships a `Doxyfile` in its root. To regenerate:

```bash
cd php-kislay-core
doxygen Doxyfile
# Output written to docs/html/
```

Requirements: Doxygen 1.9+ and Graphviz (for call graphs).

---

## PHP Extension API

The PHP-visible API (classes, methods, constants) is documented inline in each extension's `README.md` and on the relevant page of this documentation site.

| Extension | README |
|---|---|
| Core | [php-kislay-core README](https://github.com/KislayPHP/php-kislay-core#readme) |
| Gateway | [php-kislay-gateway README](https://github.com/KislayPHP/php-kislay-gateway#readme) |
| Discovery | [php-kislay-discovery README](https://github.com/KislayPHP/php-kislay-discovery#readme) |
| EventBus | [php-kislay-eventbus README](https://github.com/KislayPHP/php-kislay-eventbus#readme) |
| Queue | [php-kislay-queue README](https://github.com/KislayPHP/php-kislay-queue#readme) |
| Metrics | [php-kislay-metrics README](https://github.com/KislayPHP/php-kislay-metrics#readme) |
| Persistence | [php-kislay-persistence README](https://github.com/KislayPHP/php-kislay-persistence#readme) |
| Config | [php-kislay-config README](https://github.com/KislayPHP/php-kislay-config#readme) |
