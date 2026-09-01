# Spring Boot Mapping

KislayPHP is deliberately modelled on Spring Boot concepts — if you have a Java microservices background you will feel right at home. The table below maps the most common Spring Boot annotations and primitives to their KislayPHP equivalents.

---

## Concept Mapping Table

| Spring Boot | KislayPHP |
|---|---|
| `@SpringBootApplication` | `$app = new Kislay\Core\App()` |
| `@RestController` | PHP class with route handler methods |
| `@GetMapping("/path")` | `$app->get('/path', [$ctrl, 'method'])` |
| `@PostMapping("/path")` | `$app->post('/path', [$ctrl, 'method'])` |
| `@RequestBody` | `$req->body` |
| `@PathVariable` | `$req->params['name']` (declare the route as `:name`, not `{name}`) |
| `@RequestParam` | `$req->query['key']` |
| `@RequestHeader("X-Key")` | `$req->headers['X-Key']` |
| `@Bean` | PHP service class instantiated in bootstrap |
| `@Autowired` | Constructor injection (manual or DI container) |
| `@Service` / `@Component` | PHP class + manual wiring in `bootstrap.php` |
| `@EnableDiscoveryClient` | `$discovery = new Kislay\Discovery\ServiceRegistry()` |
| `@FeignClient("svc")` | `$client = new Kislay\ServiceClient('svc')` |
| `@Scheduled(cron="0 * * * *")` | `$app->schedule('0 * * * *', fn)` |
| `@EnableCircuitBreaker` | Gateway's process-wide circuit breaker (env vars, not a per-route option — see [API Gateway](../extensions/gateway.md)) |

---

## Example: A Complete Microservice

```php
<?php
// bootstrap.php — equivalent to @SpringBootApplication

$app       = new Kislay\Core\App();
$discovery = new Kislay\Discovery\ServiceRegistry();
$metrics   = new Kislay\Metrics\Metrics();

// Register with discovery — equivalent to @EnableDiscoveryClient
// register(name, url, ?metadata) — one URL, not separate host/port args
$discovery->register('order-service', 'http://localhost:8080');

// Controller wiring — equivalent to @RestController + @GetMapping
$ctrl = new OrderController($discovery, $metrics);
$app->get('/orders',      [$ctrl, 'index']);
$app->post('/orders',     [$ctrl, 'create']);
$app->get('/orders/:id',  [$ctrl, 'show']);

// JWT guard — equivalent to Spring Security (HS256 only)
$app->setOption('jwt_secret', getenv('JWT_SECRET'));
$app->setOption('jwt_required', true);
$app->setOption('jwt_exclude', ['/health', '/actuator']);

$app->listen('0.0.0.0', 8080);
```

---

## Key Differences

- **No annotation processor** — KislayPHP wires everything in plain PHP code rather than compile-time annotations.
- **Single process** — the C++ HTTP core runs in one long-lived PHP process; no FPM worker pool.
- **Eager start** — all extensions (Discovery, Metrics, etc.) initialise when their constructor is called, not lazily.
