# Service Discovery

## Overview

`kislayphp/discovery` is a thin service registry: register instances, track health, and resolve healthy URLs by logical name — nothing more. It deliberately does not touch business logic, request execution, JWT handling, or trace mutation.

## Installation

```bash
pie install kislayphp/discovery:1.0.1
```

```ini
extension=kislayphp_discovery.so
```

GitHub: <https://github.com/KislayPHP/discovery>

---

## Key Features

- **Register** — announce a service instance (name, URL, metadata, optional instance ID)
- **Resolve** — look up a healthy instance URL by logical name (weighted selection via `std::mt19937`, not `rand()`)
- **Standalone registry server** — a dedicated `listen()`/`run()` process other services register/resolve against over HTTP
- **Heartbeat/TTL eviction** — stale instances (no heartbeat within `KISLAY_DISCOVERY_HEARTBEAT_TIMEOUT_MS`) are pruned lazily before reads return
- **Per-service registration cap** — bounded growth (`KISLAY_DISCOVERY_MAX_INSTANCES_PER_SERVICE`)
- **Optional Redis backend** — shared registry state across nodes; falls back to in-memory (with a warning) on Redis failure, trading cluster-wide consistency for node-local safety

---

## Quick Example

Start the registry (its own process):

```php
<?php
$registry = new Kislay\Discovery\ServiceRegistry();
$registry->listen('0.0.0.0', 9010);
$registry->run();
```

Register a service instance (from that service's own process):

```php
<?php
$registry = new Kislay\Discovery\ServiceRegistry('http://127.0.0.1:9010');
$registry->register('user-service', 'http://127.0.0.1:9008', ['zone' => 'az-1'], 'user-1');
```

Resolve another service:

```php
<?php
$registry = new Kislay\Discovery\ServiceRegistry('http://127.0.0.1:9010');
$url = $registry->resolve('user-service');
```

---

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `KISLAY_DISCOVERY_HEARTBEAT_TIMEOUT_MS` | `90000` | Max heartbeat age before an instance is considered stale |
| `KISLAY_DISCOVERY_MAX_INSTANCES_PER_SERVICE` | `1024` | Per-service registration cap |
| `KISLAY_DISCOVERY_SERVER_IO_TIMEOUT_MS` | `15000` | Standalone server's per-connection read/write deadline |
| `KISLAY_DISCOVERY_STORAGE` | `memory` | `memory` or `redis` |
| `KISLAY_DISCOVERY_REDIS_HOST` / `_PORT` / `_DB` / `_TIMEOUT_MS` / `_PASSWORD` / `_PREFIX` | see README | Redis backend connection settings, only used when `KISLAY_DISCOVERY_STORAGE=redis` |
