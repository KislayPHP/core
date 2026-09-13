# Configuration

## Overview

`kislayphp/config` is a centralized configuration service for multi-node PHP deployments. It has two halves:

- **`Kislay\Config\Server`** — a standalone config store with layered scopes (global → environment → project → service → node) and an optional HTTP server other processes read from (and can push updates to).
- **`Kislay\Config\Config`** — a per-process runtime client: boot it once at startup, then read typed values through it for the rest of the process's life. It merges, in precedence order: the remote server snapshot, a local file, `setOverride()` calls, and environment variables.

There is no Consul or etcd backend — the server itself *is* the backend, over its own HTTP API.

## Installation

```bash
pie install kislayphp/config
```

Enable the extension:

```ini
extension=kislayphp_config.so
```

## Key Features

- **Typed getters** — `getString`, `getInt`, `getBool`, `getArray`, plus untyped `get`
- **Layered resolution** — global, environment, project, service, node, then local-file/env-var/runtime overrides on top
- **Environment variable overrides** — `KISLAY_CFG_` prefix by default; `__` in the name becomes `.` in the resolved key
- **Bounded remote fetch** — `Config::boot()`'s `connect_timeout_ms`/`timeout_ms` options (defaults 3000/5000ms) mean an unresponsive config server can't hang a worker indefinitely
- **File-backed cache** — `cache_file` lets a client boot from its last-known-good snapshot if the server is unreachable
- **Compatibility client** — `Kislay\Config\ConfigClient` for lightweight in-process key/value use, or delegate to your own client via `->setClient()`

## Quick Example

### 1. Start a config server

```php
<?php
$server = new Kislay\Config\Server();
$server->listen('0.0.0.0', 9011);

$server->setGlobal(['app' => ['name' => 'commerce-platform']]);
$server->setEnvironment('prod', ['app' => ['debug' => false]]);
$server->setProject('commerce', ['log' => ['level' => 'warn']]);
$server->setService('commerce', 'order-service', ['db' => ['name' => 'orders']]);
$server->setNode('commerce', 'order-service', 'order-1', ['metrics' => ['enabled' => false]]);

$server->run();
```

### 2. Load config inside a service

```php
<?php
use Kislay\Config\Config;

Config::boot([
    'server'      => 'http://127.0.0.1:9011',
    'environment' => 'prod',
    'project'     => 'commerce',
    'service'     => 'order-service',
    'node'        => 'order-1',
    'cache_file'  => '/tmp/order-service-config.json',
]);

$dbName  = Config::getString('db.name');
$debug   = Config::getBool('app.debug', true);
```

### 3. Local overrides

```php
<?php
Config::loadLocal('/etc/kislay/order-service.local.json');
Config::setOverride('gateway.timeout_ms', 1500);
```

Environment variable overrides use the `KISLAY_CFG_` prefix:

```bash
export KISLAY_CFG_DB__HOST=127.0.0.1   # resolves to key db.host
```

---

## Public API

### `Kislay\Config\Config`

```php
Config::boot(array $options): bool
Config::loadLocal(string $path): bool
Config::refresh(): bool
Config::setOverride(string $key, mixed $value): bool
Config::has(string $key): bool
Config::get(string $key, mixed $default = null): mixed
Config::getString(string $key, ?string $default = null): ?string
Config::getInt(string $key, int $default = 0): int
Config::getBool(string $key, bool $default = false): bool
Config::getArray(string $key, array $default = []): array
Config::all(): array
```

### `Kislay\Config\Server`

```php
$server = new Kislay\Config\Server(['host' => '127.0.0.1', 'port' => 9011]);
$server->listen(string $host, int $port): bool;
$server->setGlobal(array $config): bool;
$server->setEnvironment(string $environment, array $config): bool;
$server->setProject(string $project, array $config): bool;
$server->setService(string $project, string $service, array $config): bool;
$server->setNode(string $project, string $service, string $node, array $config): bool;
$server->resolve(?string $environment = null, ?string $project = null, ?string $service = null, ?string $node = null): array;
$server->run(): void;
```

### `Kislay\Config\Config::boot()` options

| Option | Default | Description |
|---|---|---|
| `server` | — | Base URL of a running `Server` (e.g. `http://127.0.0.1:9011`) |
| `environment` | — | Environment scope to resolve |
| `project` | — | Project scope to resolve |
| `service` | — | Service scope to resolve |
| `node` | — | Node scope to resolve |
| `cache_file` | — | Local file to persist/restore the last-known snapshot |
| `connect_timeout_ms` | `3000` | Non-blocking connect timeout to the config server |
| `timeout_ms` | `5000` | Socket read/write timeout for the config server request |

---

## Operational Constraints

This is an early-stage (Phase 1) release. As of now:

- No authentication on the standalone server yet.
- No background watch/poll thread — `refresh()` is a manual, explicit re-fetch, not a live push.
- No secret-masking or secret-management layer.
- No built-in rollback API for pushed config changes.
- `Config::all()` returns a flat dotted-key map, not a nested tree.

Use this release for one config node serving several services with layered environment/project/service/node overrides and fast local reads — not yet as a complete config control plane.
