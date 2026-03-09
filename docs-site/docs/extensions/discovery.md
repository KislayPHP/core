# Service Discovery

## Overview

`kislayphp/discovery` provides lightweight, in-process service registration and resolution. Microservices register themselves on startup and discover peers by logical name at request time. The extension integrates with `kislayphp/eventbus` to broadcast heartbeat and deregistration events across the cluster, and supports weighted load balancing for gradual traffic shifting.

## Installation

```bash
composer require kislayphp/discovery
```

GitHub: <https://github.com/KislayPHP/php-kislay-discovery>

---

## Key Features

- **Register** — announce a service instance (name, host, port, metadata)
- **Resolve** — look up all healthy instances by logical name
- **Weighted load balancing** — assign integer weights for canary / blue-green traffic
- **Heartbeat** — periodic keep-alive; stale instances are evicted automatically
- **EventBus integration** — publishes `service:up` and `service:down` events
- **TTL eviction** — instances not refreshed within TTL are removed
- **Metadata** — attach arbitrary key/value pairs (version, region, tags)

---

## Quick Example

```php
<?php
$discovery = new Kislay\Discovery();

// Connect to the discovery backend (built-in or external)
$discovery->connect([
    'host' => 'discovery-server',
    'port' => 7400,
]);

// Register this service instance
$discovery->register('payment-service', '10.0.1.5', 8080, [
    'version' => '1.4.2',
    'region'  => 'us-east-1',
    'weight'  => 10,
]);

// Resolve another service and pick an instance (weighted round-robin)
$instance = $discovery->resolve('order-service');
// $instance = ['host' => '10.0.1.3', 'port' => 8080, 'metadata' => [...]]

$url = "http://{$instance['host']}:{$instance['port']}/orders";

// Listen for discovery events
$discovery->on('service:down', function ($event) {
    error_log("Service went down: " . $event['name']);
});

// Deregister on shutdown
register_shutdown_function(fn () => $discovery->deregister());
```

---

## Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| `host` | string | `localhost` | Discovery server hostname |
| `port` | int | `7400` | Discovery server port |
| `ttl` | int | `30` | Instance TTL in seconds |
| `heartbeat_interval` | int | `10` | Heartbeat frequency (s) |
| `lb_strategy` | string | `weighted` | `round_robin` or `weighted` |
| `eventbus_publish` | bool | `true` | Emit EventBus events on state change |
