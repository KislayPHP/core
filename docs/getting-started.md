# Getting Started with KislayPHP

> Build your first PHP microservice in 5 minutes.

## Prerequisites

- PHP 8.2 or higher
- [PIE](https://github.com/php/pie) (PHP Extension Installer)
- Linux, macOS, or Windows

Install PIE if you don't have it:
```bash
curl -L https://github.com/php/pie/releases/latest/download/pie.phar -o /usr/local/bin/pie
chmod +x /usr/local/bin/pie
```

---

## Step 1: Install the Core Extension

```bash
pie install kislayphp/core
```

Enable in your `php.ini`:
```ini
extension=kislayphp_extension.so
```

Verify the install:
```bash
php -r "echo (new Kislay\Core\App())->version();"
```

---

## Step 2: Your First HTTP Microservice

Create `server.php`:

```php
<?php
$app = new Kislay\Core\App();
$app->setOption('threads', 4);

// Health check endpoint
$app->get('/health', function($req, $res) {
    $res->json(['status' => 'ok', 'ts' => time()]);
});

// JSON API endpoint
$app->get('/api/hello', function($req, $res) {
    $name = $req->query('name') ?? 'World';
    $res->json(['message' => "Hello, {$name}!"]);
});

// POST endpoint with body parsing
$app->post('/api/echo', function($req, $res) {
    $body = $req->getJson();
    $res->json(['echo' => $body]);
});

echo "Server running on http://0.0.0.0:8080\n";
$app->listen('0.0.0.0', 8080);
```

Run it:
```bash
php server.php
```

Test it:
```bash
curl http://localhost:8080/api/hello?name=PHP
# {"message":"Hello, PHP!"}

curl -X POST http://localhost:8080/api/echo \
     -H 'Content-Type: application/json' \
     -d '{"key":"value"}'
# {"echo":{"key":"value"}}
```

---

## Step 3: Add the Full Ecosystem

Install all extensions:
```bash
pie install kislayphp/gateway
pie install kislayphp/discovery
pie install kislayphp/metrics
pie install kislayphp/queue
pie install kislayphp/eventbus
pie install kislayphp/persistence
pie install kislayphp/config
```

Enable all in `php.ini`:
```ini
extension=kislayphp_extension.so
extension=kislayphp_gateway.so
extension=kislayphp_discovery.so
extension=kislayphp_metrics.so
extension=kislayphp_queue.so
extension=kislayphp_eventbus.so
extension=kislayphp_persistence.so
extension=kislayphp_config.so
```

---

## Step 4: Add Metrics and Observability

```php
<?php
$app     = new Kislay\Core\App();
$metrics = new Kislay\Metrics\Collector();

// Middleware: count every request
$app->use(function($req, $res, $next) use ($metrics) {
    $start = microtime(true);
    $next();
    $ms = (microtime(true) - $start) * 1000;

    $metrics->counter('http_requests_total', [
        'method' => $req->method(),
        'path'   => $req->path(),
    ])->increment();

    $metrics->histogram('http_request_duration_ms')->observe($ms);
});

// Expose Prometheus metrics
$app->get('/metrics', function($req, $res) use ($metrics) {
    $res->send($metrics->export(), 'text/plain; version=0.0.4');
});

$app->listen('0.0.0.0', 8080);
```

---

## Step 5: Add an API Gateway

```php
<?php
// gateway.php
$gateway  = new Kislay\Gateway\Gateway();
$registry = new Kislay\Discovery\Registry();

// Route all API traffic to services discovered by name
$gateway->addServiceRoute('*', '/api/users/*',   'user-service');
$gateway->addServiceRoute('*', '/api/orders/*',  'order-service');
$gateway->addServiceRoute('*', '/api/products/*','catalog-service');

$gateway->setResolver(fn($svc) => 'http://' . $registry->resolve($svc));

// Security
putenv('KISLAY_GATEWAY_AUTH_REQUIRED=1');
putenv('KISLAY_GATEWAY_AUTH_TOKEN=' . getenv('GATEWAY_SECRET'));
putenv('KISLAY_GATEWAY_RATE_LIMIT_ENABLED=1');
putenv('KISLAY_GATEWAY_RATE_LIMIT_REQUESTS=500');
putenv('KISLAY_GATEWAY_CIRCUIT_BREAKER_ENABLED=1');

echo "Gateway running on http://0.0.0.0:80\n";
$gateway->listen('0.0.0.0', 80);
```

---

## Full Architecture Example

```
                    ┌──────────────────────┐
     HTTP Traffic → │  kislayphp/gateway   │ Port 80
                    │  Rate limit + Auth   │
                    └──────────┬───────────┘
                               │ resolves via
                    ┌──────────▼───────────┐
                    │ kislayphp/discovery  │
                    └──────────┬───────────┘
            ┌──────────────────┼──────────────────┐
            ▼                  ▼                  ▼
   ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
   │ user-service   │ │ order-service  │ │catalog-service │
   │ kislayphp/core │ │ kislayphp/core │ │ kislayphp/core │
   │ + persistence  │ │ + queue        │ │ + metrics      │
   └────────────────┘ └────────────────┘ └────────────────┘
```

---

## Next Steps

- **[Core Extension](https://github.com/KislayPHP/core)** — HTTP server, async, middleware
- **[Gateway Extension](https://github.com/KislayPHP/gateway)** — API gateway, routing, auth
- **[Discovery Extension](https://github.com/KislayPHP/discovery)** — Service registry
- **[Full Documentation](https://skelves.com/kislayphp/docs)** — Complete API reference

## Getting Help

- 🐛 [Report a bug](https://github.com/KislayPHP/core/issues)
- 💬 [Ask a question](https://github.com/KislayPHP/core/discussions)
- 📖 [Full docs](https://skelves.com/kislayphp/docs)
