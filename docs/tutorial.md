# KislayPHP Tutorial: From Zero to Production Microservices

This tutorial takes you from installing `kislayphp/core` to running a complete, production-style microservices stack — gateway, service discovery, metrics, queue, event bus, persistence, and config — all in PHP with no external servers required.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Install KislayPHP Core](#2-install-kislayphp-core)
3. [Your First HTTP Server](#3-your-first-http-server)
4. [Routing, Parameters & Groups](#4-routing-parameters--groups)
5. [Middleware](#5-middleware)
6. [Async Tasks & Promises](#6-async-tasks--promises)
7. [Non-blocking HTTP with AsyncHttp](#7-non-blocking-http-with-asynchttp)
8. [HTTPS](#8-https)
9. [Complete Microservices Stack](#9-complete-microservices-stack)
   - [Service A — User Service](#service-a--user-service)
   - [Service B — Order Service](#service-b--order-service)
   - [API Gateway](#api-gateway)
   - [Metrics Sidecar](#metrics-sidecar)
   - [Queue Worker](#queue-worker)
   - [EventBus (Realtime)](#eventbus-realtime)
10. [Docker Compose Layout](#10-docker-compose-layout)
11. [Configuration Reference](#11-configuration-reference)

---

## 1. Prerequisites

- PHP 8.2+ (NTS or ZTS build)
- `phpize`, `make`, `gcc` (Linux/macOS)
- OpenSSL dev headers for HTTPS: `sudo apt install libssl-dev`

Check your PHP build:

```bash
php -i | grep "Thread Safety"
# Thread Safety => disabled  →  NTS (single-threaded HTTP, parallel async tasks serialized)
# Thread Safety => enabled   →  ZTS (full parallel HTTP + async workers)
```

---

## 2. Install KislayPHP Core

**Via PIE (recommended):**

```bash
pie install kislayphp/core
```

**From source:**

```bash
git clone https://github.com/KislayPHP/core.git
cd core
phpize
./configure --enable-kislayphp_extension
make -j$(nproc)
sudo make install
```

**Enable in `php.ini`:**

```ini
extension=kislayphp_extension.so

; Optional tuning (these are the defaults)
kislayphp.http.threads      = 4   ; HTTP server worker threads
kislayphp.http.async_threads = 4   ; background task worker pool
kislayphp.http.log          = 1
kislayphp.http.async        = 1
```

**Verify:**

```bash
php -m | grep kislayphp
# kislayphp_extension
```

---

## 3. Your First HTTP Server

```php
<?php
// server.php

$app = new Kislay\Core\App();

$app->get('/hello', function($req, $res) {
    $res->send('Hello, world!');
});

$app->get('/health', function($req, $res) {
    $res->json(['status' => 'ok', 'ts' => time()]);
});

$app->listen('0.0.0.0', 8080);
```

```bash
php server.php &
curl http://localhost:8080/hello        # Hello, world!
curl http://localhost:8080/health       # {"status":"ok","ts":...}
```

The server blocks on `listen()`. To run non-blocking (e.g., run health checks after startup):

```php
$app->listenAsync('0.0.0.0', 8080);

// Do startup work here — server is already accepting connections
echo "Server started\n";

$app->wait(); // block until shutdown
```

---

## 4. Routing, Parameters & Groups

### URL parameters

```php
$app->get('/users/:id', function($req, $res) {
    $id = $req->input('id');          // route parameter
    $res->json(['id' => $id]);
});

// Wildcard
$app->get('/files/*', function($req, $res) {
    $res->send('file: ' . $req->path());
});
```

### Query string & body

```php
$app->get('/search', function($req, $res) {
    $params = $req->getQueryParams();  // ['q' => 'php', 'page' => '2']
    $q      = $req->input('q', '');   // works for both query + body
    $res->json(['query' => $q, 'page' => $params['page'] ?? 1]);
});

$app->post('/users', function($req, $res) {
    $data = $req->getJson();           // parsed JSON body
    // OR: $req->all()                 // form fields
    $res->created(['id' => 1, ...$data]);
});
```

### Route groups

```php
$app->group('/api/v1', function($app) {
    $app->group('/users', function($app) {
        $app->get('/',      fn($q,$s) => $s->json(getUsers()));
        $app->get('/:id',   fn($q,$s) => $s->json(getUserById($q->input('id'))));
        $app->post('/',     fn($q,$s) => $s->created(createUser($q->getJson())));
        $app->put('/:id',   fn($q,$s) => $s->json(updateUser($q->input('id'), $q->getJson())));
        $app->delete('/:id',fn($q,$s) => $s->noContent());
    });

    $app->group('/orders', function($app) {
        $app->get('/',    fn($q,$s) => $s->json(getOrders()));
        $app->post('/',   fn($q,$s) => $s->created(createOrder($q->getJson())));
    });
});
```

### All convenience response methods

```php
$res->ok($data)                 // 200
$res->created($data)            // 201
$res->noContent()               // 204
$res->badRequest('msg')         // 400
$res->unauthorized('msg')       // 401
$res->forbidden('msg')          // 403
$res->notFound('msg')           // 404
$res->conflict('msg')           // 409
$res->internalServerError('msg')// 500
```

---

## 5. Middleware

Middleware runs before your route handlers. Use it for auth, logging, rate limiting, request ID injection, etc.

### Global middleware

```php
$app->use(function($req, $res, $next) {
    // Inject request ID for distributed tracing
    $reqId = $req->header('x-request-id', uniqid('req-'));
    $req->setAttribute('request_id', $reqId);
    $res->set('x-request-id', $reqId);
    $next();
});
```

### Path-scoped middleware

```php
// Only runs for /api/* routes
$app->use('/api', function($req, $res, $next) {
    $res->set('X-Powered-By', 'KislayPHP');
    $next();
});
```

### Auth middleware (reusable)

```php
$requireAuth = function($req, $res, $next) {
    $token = $req->header('authorization', '');
    if (!str_starts_with($token, 'Bearer ') || !validateToken(substr($token, 7))) {
        $res->unauthorized('Invalid or missing token');
        return;  // do NOT call $next() — stops chain
    }
    $req->setAttribute('user', decodeToken(substr($token, 7)));
    $next();
};

// Apply to a whole group
$app->group('/api/admin', function($app) {
    $app->get('/dashboard', fn($q,$s) => $s->json(getDashboard()));
    $app->get('/users',     fn($q,$s) => $s->json(getAllUsers()));
}, [$requireAuth]);
```

### Request lifecycle hooks

Use these when you need begin/cleanup that wraps the entire handler (e.g., DB transactions):

```php
$app->onRequestStart(function($req, $res) {
    $req->setAttribute('start_ms', microtime(true) * 1000);
});

$app->onRequestEnd(function($req, $res) {
    $dur = round(microtime(true) * 1000 - $req->getAttribute('start_ms', 0), 2);
    error_log("[kislay] {$req->method()} {$req->path()} → {$dur}ms");
});
```

---

## 6. Async Tasks & Promises

KislayPHP Core has a built-in multi-threaded worker pool (enabled by default). Offload CPU-heavy or I/O-bound work without blocking the HTTP thread.

```php
$app->setOption('async_threads', 4); // 4 background workers

$app->get('/report', function($req, $res) {
    // Return immediately — report runs in background
    async(function() {
        return buildHeavyReport();         // runs in worker thread
    })->then(function($report) {
        saveReport($report);               // called when done
        echo "Report ready\n";
    })->catch(function($err) {
        error_log("Report failed: " . $err->getMessage());
    })->finally(function() {
        echo "Report task cleaned up\n";
    });

    $res->json(['status' => 'report queued']);
});
```

**Promise chaining:**

```php
async(fn() => fetchUser($id))
    ->then(fn($user) => enrichWithProfile($user))
    ->then(fn($user) => saveToCache($user))
    ->catch(fn($e)   => error_log($e->getMessage()));
```

> **NTS PHP**: Workers exist but PHP execution is serialized (mutex). Best for I/O-bound tasks (network waits).
> **ZTS PHP**: Each worker gets its own PHP context — true parallelism. Best for CPU-bound tasks.

---

## 7. Non-blocking HTTP with AsyncHttp

Make outbound HTTP requests without blocking the server:

```php
$app->get('/aggregate', function($req, $res) {
    $http = new Kislay\Core\AsyncHttp();

    // Configure request
    $http->get('https://api.github.com/repos/KislayPHP/core');
    $http->setHeader('User-Agent', 'KislayPHP');
    $http->retry(3, 500);  // retry 3 times, 500ms delay

    $http->executeAsync()->then(function() use ($http, $res) {
        $code = $http->getResponseCode();
        $body = $http->getResponse();
        $data = json_decode($body, true);
        // Note: response handling happens in background thread
    });

    $res->json(['status' => 'fetching']);
});
```

**Correlation ID propagation** is automatic: `AsyncHttp` picks up `X-Correlation-ID` from the incoming request and forwards it to all outgoing calls. If none exists, it generates one.

---

## 8. HTTPS

```php
$app->listen('0.0.0.0', 8443, [
    'cert' => '/etc/ssl/certs/server.crt',
    'key'  => '/etc/ssl/private/server.key',
]);
```

Or via environment:

```bash
export KISLAYPHP_HTTP_TLS_CERT=/etc/ssl/certs/server.crt
export KISLAYPHP_HTTP_TLS_KEY=/etc/ssl/private/server.key
```

Self-signed cert for development:

```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes \
  -subj '/CN=localhost'
```

---

## 9. Complete Microservices Stack

This section builds a realistic microservices system:

```
Browser / curl
     │
     ▼
┌──────────┐      ┌──────────────┐      ┌──────────────┐
│  Gateway  │────▶│ User Service  │      │ Order Service │
│  :9008    │     │   :9001       │      │   :9002       │
└──────────┘      └──────────────┘      └──────────────┘
     │                  │                      │
     │         ┌────────┴──────────────────────┤
     │         ▼                               ▼
     │   ┌──────────┐                   ┌──────────┐
     │   │ Discovery│                   │  Queue   │
     │   │ Registry │                   │  Worker  │
     │   └──────────┘                   └──────────┘
     │
     ▼
┌──────────┐
│ EventBus │  (realtime push to browser)
│  :3000   │
└──────────┘
```

Install all extensions:

```bash
pie install kislayphp/core kislayphp/gateway kislayphp/discovery \
            kislayphp/metrics kislayphp/queue kislayphp/eventbus \
            kislayphp/persistence kislayphp/config
```

`php.ini`:

```ini
extension=kislayphp_extension.so
extension=kislayphp_gateway.so
extension=kislayphp_discovery.so
extension=kislayphp_metrics.so
extension=kislayphp_queue.so
extension=kislayphp_eventbus.so   ; (extension name: kislayphp_socket.so)
extension=kislayphp_persistence.so
extension=kislayphp_config.so
```

---

### Service A — User Service

`services/user-service.php`

```php
<?php
// User Service — listens on :9001

$config = new Kislay\Config\Config();
$config->set('service.name', 'user-service');
$config->set('service.host', getenv('HOST') ?: '127.0.0.1');
$config->set('service.port', (int)(getenv('PORT') ?: 9001));

$metrics = new Kislay\Metrics\Collector();
$registry = new Kislay\Discovery\Registry();
$queue    = new Kislay\Queue\Queue();

// --- App ---
$app = new Kislay\Core\App();
$app->setOption('threads', 4);

// Persistence lifecycle per request (auto begin/commit/rollback)
Kislay\Persistence\Runtime::attach($app);

// Per-request timing
$app->onRequestStart(fn($req) => $req->setAttribute('t0', microtime(true)));
$app->onRequestEnd(function($req) use ($metrics) {
    $ms = (microtime(true) - $req->getAttribute('t0', microtime(true))) * 1000;
    $metrics->histogram('request_duration_ms', ['service' => 'user'])->observe($ms);
    $metrics->counter('requests_total', [
        'service' => 'user',
        'method'  => $req->method(),
    ])->increment();
});

// Routes
$app->group('/api/users', function($app) use ($queue, $metrics) {

    $app->get('/', function($req, $res) {
        // In real usage: query your DB here
        $res->json([
            ['id' => 1, 'name' => 'Alice', 'email' => 'alice@example.com'],
            ['id' => 2, 'name' => 'Bob',   'email' => 'bob@example.com'],
        ]);
    });

    $app->get('/:id', function($req, $res) {
        $id = (int) $req->input('id');
        // DB::find('users', $id) in real usage
        $res->json(['id' => $id, 'name' => 'Alice', 'email' => 'alice@example.com']);
    });

    $app->post('/', function($req, $res) use ($queue) {
        $data = $req->getJson();

        if (empty($data['email'])) {
            $res->badRequest('email is required');
            return;
        }

        // In real usage: DB::create('users', $data)
        $newId = rand(100, 999);

        // Enqueue welcome email (async, non-blocking)
        $queue->enqueue('email-jobs', [
            'type'  => 'welcome',
            'to'    => $data['email'],
            'name'  => $data['name'] ?? 'New User',
        ]);

        $res->created(['id' => $newId, ...$data]);
    });

    $app->delete('/:id', function($req, $res) {
        $id = (int) $req->input('id');
        // DB::delete('users', $id) in real usage
        $res->noContent();
    });
});

// Metrics endpoint (scraped by Prometheus)
$app->get('/metrics', fn($req, $res) =>
    $res->send($metrics->export(), 'text/plain; version=0.0.4')
);

// Health
$app->get('/health', fn($req, $res) => $res->json(['ok' => true]));

// Register in service registry
$registry->register(
    $config->get('service.name'),
    $config->get('service.host'),
    $config->get('service.port')
);

register_shutdown_function(function() use ($registry, $config) {
    $registry->deregister(
        $config->get('service.name'),
        $config->get('service.host'),
        $config->get('service.port')
    );
    echo "[user-service] deregistered\n";
});

echo "[user-service] Starting on port " . $config->get('service.port') . "\n";
$app->listen($config->get('service.host'), $config->get('service.port'));
```

---

### Service B — Order Service

`services/order-service.php`

```php
<?php
// Order Service — listens on :9002

$config   = new Kislay\Config\Config();
$config->set('service.name', 'order-service');
$config->set('service.host', getenv('HOST') ?: '127.0.0.1');
$config->set('service.port', (int)(getenv('PORT') ?: 9002));

$metrics  = new Kislay\Metrics\Collector();
$registry = new Kislay\Discovery\Registry();
$queue    = new Kislay\Queue\Queue();

$app = new Kislay\Core\App();
$app->setOption('threads', 4);
$app->setOption('async_threads', 4);

Kislay\Persistence\Runtime::attach($app);

$app->onRequestEnd(function($req) use ($metrics) {
    $metrics->counter('requests_total', ['service' => 'order'])->increment();
});

$app->group('/api/orders', function($app) use ($queue, $registry, $metrics) {

    $app->get('/', fn($req, $res) => $res->json([
        ['id' => 1, 'user_id' => 1, 'total' => 99.99,  'status' => 'paid'],
        ['id' => 2, 'user_id' => 2, 'total' => 149.00, 'status' => 'pending'],
    ]));

    $app->post('/', function($req, $res) use ($queue, $registry, $metrics) {
        $data = $req->getJson();

        if (empty($data['user_id']) || empty($data['items'])) {
            $res->badRequest('user_id and items required');
            return;
        }

        // Validate user exists — call user-service via discovery
        $userAddr = $registry->resolve('user-service');
        $http = new Kislay\Core\AsyncHttp();
        $http->get("http://{$userAddr}/api/users/{$data['user_id']}");
        if (!$http->execute() || $http->getResponseCode() !== 200) {
            $res->badRequest('user not found');
            return;
        }

        $orderId = rand(1000, 9999);

        // Queue fulfillment job
        $queue->enqueue('fulfillment-jobs', [
            'order_id' => $orderId,
            'user_id'  => $data['user_id'],
            'items'    => $data['items'],
        ]);

        $metrics->counter('orders_created_total')->increment();

        $res->created(['id' => $orderId, 'status' => 'processing', ...$data]);
    });

    $app->get('/:id', function($req, $res) {
        $id = (int) $req->input('id');
        $res->json(['id' => $id, 'status' => 'paid', 'total' => 99.99]);
    });
});

$app->get('/health',  fn($q,$s) => $s->json(['ok' => true]));
$app->get('/metrics', fn($q,$s) => $s->send($metrics->export(), 'text/plain; version=0.0.4'));

$registry->register(
    $config->get('service.name'),
    $config->get('service.host'),
    $config->get('service.port')
);

register_shutdown_function(function() use ($registry, $config) {
    $registry->deregister(
        $config->get('service.name'),
        $config->get('service.host'),
        $config->get('service.port')
    );
});

echo "[order-service] Starting on port " . $config->get('service.port') . "\n";
$app->listen($config->get('service.host'), $config->get('service.port'));
```

---

### API Gateway

`gateway.php`

```php
<?php
// API Gateway — listens on :9008, routes all traffic

$registry = new Kislay\Discovery\Registry();
$metrics  = new Kislay\Metrics\Collector();
$gateway  = new Kislay\Gateway\Gateway();

// Environment-driven security
putenv('KISLAY_GATEWAY_AUTH_REQUIRED=1');
putenv('KISLAY_GATEWAY_AUTH_TOKEN=' . (getenv('API_SECRET') ?: 'dev-secret'));
putenv('KISLAY_GATEWAY_AUTH_EXCLUDE=/health,/metrics');

// Rate limiting
putenv('KISLAY_GATEWAY_RATE_LIMIT_ENABLED=1');
putenv('KISLAY_GATEWAY_RATE_LIMIT_REQUESTS=200');
putenv('KISLAY_GATEWAY_RATE_LIMIT_WINDOW=60');

// Circuit breaker
putenv('KISLAY_GATEWAY_CIRCUIT_BREAKER_ENABLED=1');
putenv('KISLAY_GATEWAY_CB_FAILURE_THRESHOLD=5');
putenv('KISLAY_GATEWAY_CB_OPEN_SECONDS=30');

// Routes — logical service name, resolved dynamically
$gateway->addServiceRoute('GET',    '/api/users',     'user-service');
$gateway->addServiceRoute('GET',    '/api/users/*',   'user-service');
$gateway->addServiceRoute('POST',   '/api/users',     'user-service');
$gateway->addServiceRoute('DELETE', '/api/users/*',   'user-service');

$gateway->addServiceRoute('GET',    '/api/orders',    'order-service');
$gateway->addServiceRoute('GET',    '/api/orders/*',  'order-service');
$gateway->addServiceRoute('POST',   '/api/orders',    'order-service');

// Discovery-backed resolver
$gateway->setResolver(function(string $service) use ($registry): string {
    $addr = $registry->resolve($service);
    return "http://{$addr}";
});

// Fallback for unmatched routes
$gateway->setFallbackTarget('http://127.0.0.1:9001');

echo "[gateway] Starting on :9008\n";
$gateway->listen('0.0.0.0', 9008);
```

**Test the gateway:**

```bash
curl -H "Authorization: Bearer dev-secret" http://localhost:9008/api/users
curl -H "Authorization: Bearer dev-secret" http://localhost:9008/api/orders
```

---

### Metrics Sidecar

Each service already exposes `/metrics`. Scrape them from Prometheus using:

```yaml
# prometheus.yml
scrape_configs:
  - job_name: kislayphp
    static_configs:
      - targets:
          - localhost:9001   # user-service
          - localhost:9002   # order-service
```

Or collect them yourself:

```php
<?php
// metrics-aggregator.php — simple polling aggregator

$services = [
    'user-service'  => 'http://127.0.0.1:9001/metrics',
    'order-service' => 'http://127.0.0.1:9002/metrics',
];

foreach ($services as $name => $url) {
    $http = new Kislay\Core\AsyncHttp();
    $http->get($url);
    if ($http->execute() && $http->getResponseCode() === 200) {
        echo "=== $name ===\n" . $http->getResponse() . "\n";
    }
}
```

---

### Queue Worker

`workers/email-worker.php`

```php
<?php
// Email Queue Worker — runs as a separate process

$queue = new Kislay\Queue\Queue();
// For production: $queue->setClient(new RedisQueueClient(new Redis()));

echo "[email-worker] Listening on 'email-jobs'\n";

while (true) {
    $job = $queue->dequeue('email-jobs');

    if ($job === null) {
        usleep(100_000); // 100ms back-off when empty
        continue;
    }

    echo "[email-worker] Processing: {$job['type']} → {$job['to']}\n";

    try {
        match ($job['type']) {
            'welcome' => sendWelcomeEmail($job['to'], $job['name']),
            'invoice' => sendInvoiceEmail($job['to'], $job['order_id']),
            default   => error_log("Unknown job type: {$job['type']}"),
        };
    } catch (\Throwable $e) {
        error_log("[email-worker] Job failed: " . $e->getMessage());
        // Re-enqueue with retry count in production
    }
}

function sendWelcomeEmail(string $to, string $name): void {
    // Your mailer here (PHPMailer, Symfony Mailer, etc.)
    echo "  Sent welcome email to $to\n";
}

function sendInvoiceEmail(string $to, int $orderId): void {
    echo "  Sent invoice email to $to for order #$orderId\n";
}
```

**Fulfillment worker:**

`workers/fulfillment-worker.php`

```php
<?php
$queue    = new Kislay\Queue\Queue();
$eventbus = new Kislay\EventBus\Server();  // push realtime events to connected browsers

echo "[fulfillment-worker] Running\n";

while (true) {
    $job = $queue->dequeue('fulfillment-jobs');

    if ($job === null) {
        usleep(200_000);
        continue;
    }

    echo "[fulfillment-worker] Fulfilling order #{$job['order_id']}\n";

    // Simulate fulfillment work
    sleep(1);

    // Push realtime update to user via EventBus
    $eventbus->emitTo(
        "user-{$job['user_id']}",
        'order_status',
        ['order_id' => $job['order_id'], 'status' => 'shipped']
    );

    echo "  Order #{$job['order_id']} fulfilled and user notified\n";
}
```

---

### EventBus (Realtime)

`eventbus-server.php`

```php
<?php
// EventBus Server — WebSocket-compatible realtime push on :3000

$server = new Kislay\EventBus\Server();

$server->on('connection', function(Kislay\EventBus\Socket $socket) use ($server) {
    echo "[eventbus] Client connected: " . $socket->id() . "\n";

    // Client authenticates and joins their personal room
    $socket->on('auth', function($data) use ($socket) {
        if (empty($data['user_id'])) {
            $socket->reply('error', ['message' => 'user_id required']);
            return;
        }
        $userId = (int) $data['user_id'];
        $socket->join("user-{$userId}");
        $socket->reply('authenticated', ['room' => "user-{$userId}"]);
        echo "  User $userId joined room user-{$userId}\n";
    });

    // Chat / broadcast messages
    $socket->on('broadcast', function($data) use ($server) {
        $server->emit('broadcast', [
            'from'    => $data['from'] ?? 'anonymous',
            'message' => $data['message'] ?? '',
            'ts'      => time(),
        ]);
    });

    // System-wide announcements
    $socket->on('announce', function($data) use ($server) {
        $server->emitTo('announcements', 'announcement', $data);
    });
});

echo "[eventbus] Realtime server on :3000/events/\n";
$server->listen('0.0.0.0', 3000, '/events/');
```

**Browser client (JavaScript):**

```javascript
const ws = new WebSocket('ws://localhost:3000/events/');

ws.onopen = () => {
    // Authenticate
    ws.send(JSON.stringify({ event: 'auth', data: { user_id: 42 } }));
};

ws.onmessage = ({ data }) => {
    const { event, payload } = JSON.parse(data);
    if (event === 'order_status') {
        console.log(`Order #${payload.order_id} is now: ${payload.status}`);
    }
    if (event === 'authenticated') {
        console.log('Connected to room:', payload.room);
    }
};
```

---

## 10. Docker Compose Layout

```yaml
# docker-compose.yml
version: '3.9'

services:
  gateway:
    image: php:8.2-cli
    command: php /app/gateway.php
    ports: ["9008:9008"]
    environment:
      API_SECRET: ${API_SECRET:-dev-secret}
    volumes: [./:/app]
    depends_on: [user-service, order-service]

  user-service:
    image: php:8.2-cli
    command: php /app/services/user-service.php
    environment:
      HOST: "0.0.0.0"
      PORT: "9001"
    ports: ["9001:9001"]
    volumes: [./:/app]

  order-service:
    image: php:8.2-cli
    command: php /app/services/order-service.php
    environment:
      HOST: "0.0.0.0"
      PORT: "9002"
    ports: ["9002:9002"]
    volumes: [./:/app]

  email-worker:
    image: php:8.2-cli
    command: php /app/workers/email-worker.php
    volumes: [./:/app]

  fulfillment-worker:
    image: php:8.2-cli
    command: php /app/workers/fulfillment-worker.php
    volumes: [./:/app]

  eventbus:
    image: php:8.2-cli
    command: php /app/eventbus-server.php
    ports: ["3000:3000"]
    volumes: [./:/app]
```

**Start the full stack:**

```bash
docker-compose up
```

**Smoke test:**

```bash
# Create a user
curl -s -X POST http://localhost:9008/api/users \
  -H "Authorization: Bearer dev-secret" \
  -H "Content-Type: application/json" \
  -d '{"name":"Alice","email":"alice@example.com"}' | jq

# List users
curl -s http://localhost:9008/api/users \
  -H "Authorization: Bearer dev-secret" | jq

# Create an order
curl -s -X POST http://localhost:9008/api/orders \
  -H "Authorization: Bearer dev-secret" \
  -H "Content-Type: application/json" \
  -d '{"user_id":1,"items":[{"sku":"PROD-1","qty":2}]}' | jq

# Check service health directly
curl http://localhost:9001/health
curl http://localhost:9002/health

# Prometheus metrics
curl http://localhost:9001/metrics
```

---

## 11. Configuration Reference

### Core (`Kislay\Core\App`)

| `setOption` key | Default | Description |
|---|---|---|
| `threads` | `4` | HTTP server worker threads. Clamped to `1` on NTS PHP. |
| `async_threads` | `4` | Background task worker pool size. |
| `async` | `true` | Enable async worker pool (v0.0.2+ default). |
| `log` | `true` | Request logging (`[kislay] time=... request=...`). |
| `cors` | `false` | Enable CORS headers. |
| `max_body` | `0` (unlimited) | Max request body size in bytes. |
| `document_root` | `""` | Static file serving root. |
| `referrer_policy` | `strict-origin-when-cross-origin` | Referrer policy header. |

### Gateway (`Kislay\Gateway\Gateway`) — Environment Variables

| Variable | Default | Description |
|---|---|---|
| `KISLAY_GATEWAY_AUTH_REQUIRED` | `0` | Enable JWT/bearer auth |
| `KISLAY_GATEWAY_AUTH_TOKEN` | — | Expected bearer token |
| `KISLAY_GATEWAY_AUTH_EXCLUDE` | — | Comma-separated paths excluded from auth |
| `KISLAY_GATEWAY_RATE_LIMIT_ENABLED` | `0` | Enable rate limiter |
| `KISLAY_GATEWAY_RATE_LIMIT_REQUESTS` | `100` | Max requests per window |
| `KISLAY_GATEWAY_RATE_LIMIT_WINDOW` | `60` | Window in seconds |
| `KISLAY_GATEWAY_CIRCUIT_BREAKER_ENABLED` | `0` | Enable circuit breaker |
| `KISLAY_GATEWAY_CB_FAILURE_THRESHOLD` | `5` | Failures before open |
| `KISLAY_GATEWAY_CB_OPEN_SECONDS` | `30` | Cooldown before retry |

### php.ini Tuning

```ini
; Core
kislayphp.http.threads       = 4
kislayphp.http.async_threads = 4
kislayphp.http.log           = 1
kislayphp.http.cors          = 0
kislayphp.http.max_body      = 10485760   ; 10MB
kislayphp.http.read_timeout_ms = 10000

; TLS
kislayphp.http.tls_cert = /path/to/cert.pem
kislayphp.http.tls_key  = /path/to/key.pem
```

---

## Summary

| Extension | Role | Port |
|---|---|---|
| `kislayphp/core` | HTTP server, routing, async | any |
| `kislayphp/gateway` | Reverse proxy, auth, rate limit, circuit breaker | 9008 |
| `kislayphp/discovery` | Service registry (register + resolve) | in-process |
| `kislayphp/metrics` | Counters, gauges, histograms, Prometheus export | /metrics |
| `kislayphp/queue` | Job queue with pluggable backend | in-process |
| `kislayphp/eventbus` | Realtime WebSocket-style push | 3000 |
| `kislayphp/persistence` | Per-request transaction lifecycle | in-process |
| `kislayphp/config` | Namespace-scoped config store | in-process |

**Full docs:** [https://skelves.com/kislayphp/docs](https://skelves.com/kislayphp/docs)
