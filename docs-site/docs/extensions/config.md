# Configuration

## Overview

`kislayphp/config` provides a typed, live-reloadable configuration layer that reads from environment variables, `.env` files, Consul KV, or etcd. Values are cached in a C++ hash map for zero-overhead reads in the request path; a background watcher thread polls remote backends and hot-reloads changed keys without a process restart.

## Installation

```bash
composer require kislayphp/config
```

GitHub: <https://github.com/KislayPHP/php-kislay-config>

---

## Key Features

- **Typed getters** — `getString`, `getInt`, `getBool`, `getFloat`, `getArray`
- **Default values** — all getters accept a fallback
- **Multiple backends** — `.env` / environment, Consul KV, etcd v3
- **Live refresh** — background thread detects changes and updates the cache
- **Change listeners** — subscribe to key or prefix change events
- **Namespace support** — prefix keys with service name to avoid collisions
- **Secret masking** — keys matching patterns are redacted from `/actuator/info`

---

## Quick Example

```php
<?php
$config = new Kislay\Config();

// Load from .env + environment (default)
$config->load(['backend' => 'env', 'file' => '.env']);

// Or load from Consul
$config->load([
    'backend'    => 'consul',
    'host'       => 'consul:8500',
    'prefix'     => 'myapp/',
    'refresh_ms' => 5000,
]);

// Typed getters
$port    = $config->getInt('SERVER_PORT', 8080);
$debug   = $config->getBool('APP_DEBUG', false);
$dsn     = $config->getString('DATABASE_URL');
$allowed = $config->getArray('ALLOWED_ORIGINS', []);

// Change listener
$config->onChange('FEATURE_FLAG_', function ($key, $newValue) {
    echo "Feature flag changed: $key = $newValue\n";
});

// Set at runtime
$config->set('CACHE_TTL', 300);

// Refresh all values from backend immediately
$config->refresh();
```

---

## Configuration

| Option | Default | Description |
|---|---|---|
| `backend` | `env` | `env`, `consul`, or `etcd` |
| `file` | `.env` | Path to .env file (env backend) |
| `host` | `localhost` | Backend host |
| `port` | `8500` / `2379` | Backend port |
| `prefix` | — | Key prefix to scope reads |
| `refresh_ms` | `10000` | Poll interval for remote backends |
| `secret_patterns` | `['_SECRET', '_PASSWORD', '_TOKEN']` | Patterns redacted from actuator |
