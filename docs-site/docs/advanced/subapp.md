# Sub-app Mounting

`$app->mount()` lets you attach a child `Kislay\App` instance at a URI prefix. The mounted sub-app inherits the parent's middleware pipeline but registers its own routes independently.

---

## How It Works

When a request matches the mount prefix, the core strips the prefix and dispatches the remaining path to the sub-app's own router. This means a route defined as `/users` inside the sub-app responds to `/api/users` externally.

---

## Example

```php
<?php
// --- Sub-app: API v1 ---
$apiV1 = new Kislay\App();

$apiV1->get('/users',      fn ($req, $res) => $res->json(['v' => 1, 'resource' => 'users']));
$apiV1->get('/orders',     fn ($req, $res) => $res->json(['v' => 1, 'resource' => 'orders']));


// --- Sub-app: API v2 ---
$apiV2 = new Kislay\App();

$apiV2->get('/users',      fn ($req, $res) => $res->json(['v' => 2, 'resource' => 'users']));
$apiV2->get('/orders',     fn ($req, $res) => $res->json(['v' => 2, 'resource' => 'orders']));


// --- Root app ---
$app = new Kislay\App();
$app->setOption('port', 8080);

// Global middleware applied to all sub-apps
$app->use(function ($req, $res, $next) {
    $res->header('X-Api-Version', 'mixed');
    $next();
});

// Mount the sub-apps
$app->mount('/api/v1', $apiV1);   // /api/v1/users, /api/v1/orders
$app->mount('/api/v2', $apiV2);   // /api/v2/users, /api/v2/orders

// Root route still works
$app->get('/health', fn ($req, $res) => $res->json(['ok' => true]));

$app->listen();
```

---

## URI Stripping Detail

| External path | Mounted at | Sub-app receives |
|---|---|---|
| `/api/v1/users` | `/api/v1` | `/users` |
| `/api/v1/orders/42` | `/api/v1` | `/orders/42` |
| `/api/v2/users?page=2` | `/api/v2` | `/users?page=2` |

Query strings are passed through unchanged.

---

## Notes

- Sub-apps can themselves call `mount()` for deeper nesting.
- Each sub-app has its own middleware stack; parent middleware runs first.
- Route parameters defined before `mount()` are available in `$req->params` inside the sub-app.
