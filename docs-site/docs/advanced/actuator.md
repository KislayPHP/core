# Actuator Endpoints

When `actuator` is enabled, KislayPHP automatically registers a set of management endpoints inspired by Spring Boot Actuator — no additional code required.

---

## Enabling Actuator

```php
<?php
$app = new Kislay\App();
$app->setOption('actuator', true);
$app->setOption('port', 8080);
$app->listen();
```

---

## Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/actuator/ping` | GET | Returns `{"pong":true}` — simple liveness |
| `/actuator/health` | GET | Aggregate health status of the process |
| `/actuator/info` | GET | App name, version, environment (from Config ext) |
| `/actuator/metrics` | GET | Basic process metrics (memory, uptime, req count) |
| `/actuator/routes` | GET | All registered routes and their handler types |

---

## Example Responses

### `/actuator/health`

```json
{
  "status": "UP",
  "components": {
    "db":        { "status": "UP" },
    "discovery": { "status": "UP", "registered": true },
    "queue":     { "status": "UP", "depth": 12 }
  }
}
```

### `/actuator/info`

```json
{
  "app": {
    "name":    "order-service",
    "version": "1.4.2",
    "env":     "production"
  },
  "php": { "version": "8.2.10" },
  "kislay": { "core": "0.9.1" }
}
```

### `/actuator/routes`

```json
[
  { "method": "GET",  "path": "/orders",      "handler": "OrderController@index" },
  { "method": "POST", "path": "/orders",      "handler": "OrderController@store" },
  { "method": "GET",  "path": "/orders/{id}", "handler": "Closure" }
]
```

---

## Securing Actuator

Combine with JWT exclusion to keep actuator accessible from internal networks only:

```php
$app->setOption('jwt_exclude', ['/actuator']);

// Restrict actuator to internal IP via middleware
$app->use('/actuator', function ($req, $res, $next) {
    if (!str_starts_with($req->headers['X-Forwarded-For'] ?? $req->ip, '10.')) {
        return $res->status(403)->json(['error' => 'Forbidden']);
    }
    $next();
});
```
