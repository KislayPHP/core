# Quick Start

This guide walks you through the most common patterns you will use daily with KislayPHP.

## Hello World

```php
<?php
require __DIR__ . '/vendor/autoload.php';

$app = new Kislay\App();
$app->setOption('port', 8080);

$app->get('/', function ($req, $res) {
    $res->json(['message' => 'Hello, World!']);
});

$app->listen();
```

```bash
php server.php
# Server listening on http://0.0.0.0:8080
```

---

## Routes

KislayPHP supports all standard HTTP verbs:

```php
$app->get('/users',         [$ctrl, 'index']);
$app->post('/users',        [$ctrl, 'store']);
$app->put('/users/{id}',    [$ctrl, 'update']);
$app->delete('/users/{id}', [$ctrl, 'destroy']);
$app->patch('/users/{id}',  [$ctrl, 'patch']);
```

---

## Route Parameters

Named segments prefixed with `{` and `}` are captured into `$req->params`:

```php
$app->get('/orders/{orderId}/items/{itemId}', function ($req, $res) {
    $orderId = $req->params['orderId'];
    $itemId  = $req->params['itemId'];
    $res->json(['order' => $orderId, 'item' => $itemId]);
});
```

---

## Query String

Query parameters are available via `$req->query`:

```php
$app->get('/search', function ($req, $res) {
    $term  = $req->query['q']    ?? '';
    $limit = $req->query['limit'] ?? 20;
    $res->json(['term' => $term, 'limit' => (int)$limit]);
});
```

---

## Request Body

For `POST` / `PUT` / `PATCH` requests, the parsed JSON body is in `$req->body`:

```php
$app->post('/users', function ($req, $res) {
    $name  = $req->body['name']  ?? null;
    $email = $req->body['email'] ?? null;

    if (!$name || !$email) {
        return $res->status(400)->json(['error' => 'name and email required']);
    }

    // persist...
    $res->status(201)->json(['created' => true]);
});
```

---

## Middleware

Middleware receives three arguments: `$req`, `$res`, and `$next`. Call `$next()` to continue the chain.

```php
// Logging middleware
$app->use(function ($req, $res, $next) {
    error_log($req->method . ' ' . $req->path);
    $next();
});

// Auth middleware on a specific route
$app->get('/admin', $authMiddleware, function ($req, $res) {
    $res->json(['admin' => true]);
});
```

---

## Error Handling

A four-parameter middleware catches errors thrown or passed to `$next($error)`:

```php
$app->use(function ($err, $req, $res, $next) {
    error_log('Unhandled: ' . $err->getMessage());
    $res->status(500)->json(['error' => 'Internal Server Error']);
});
```

---

## JSON Responses

`$res->json()` sets `Content-Type: application/json` and encodes the payload:

```php
$res->json(['ok' => true]);                       // 200
$res->status(201)->json(['id' => $newId]);        // 201 Created
$res->status(404)->json(['error' => 'Not found']); // 404
```

---

## Running

```bash
php server.php
```

Use a process manager like **Supervisor** or **systemd** in production so the long-running process is automatically restarted on failure.
