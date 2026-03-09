# Spring Boot → KislayPHP Mapping Guide

> For developers familiar with Spring Boot migrating to or evaluating KislayPHP

## Core Philosophy

KislayPHP follows the same layered architecture as Spring Boot:
- **C++ extensions** handle all infrastructure (equivalent to Spring's auto-configuration)
- **PHP** handles only business logic (equivalent to @Service, @Controller classes)
- Zero framework XML/YAML — configuration via env vars or PHP arrays

---

## Application Bootstrap

| Spring Boot | KislayPHP |
|---|---|
| `@SpringBootApplication` | `$app = new \Kislay\Core\App()` |
| `SpringApplication.run()` | `$app->listen(8080)` |
| `application.properties` | `setOption()` + env vars |
| `application.yml` | Same |
| `--server.port=8080` | `KISLAY_HTTP_PORT=8080` env or `listen(8080)` |

---

## HTTP Layer

| Spring Boot | KislayPHP |
|---|---|
| `@RestController` | Route handlers (closures or `[Class, 'method']` array syntax) |
| `@GetMapping("/users")` | `$app->get('/users', fn($req,$res) => ...)` |
| `@PostMapping` | `$app->post(...)` |
| `@RequestBody` | `$req->body()` (auto-parsed JSON/form) |
| `@PathVariable` | `$req->param('id')` |
| `@RequestParam` | `$req->query('page')` |
| `@RequestHeader` | `$req->header('Authorization')` |
| `ResponseEntity` | `$res->status(201)->json([...])` |
| `@ControllerAdvice` / `@ExceptionHandler` | `$app->onError(fn($err,$req,$res) => ...)` |
| `HandlerInterceptorAdapter` | `$app->use(fn($req,$res,$next) => ...)` |
| `@CrossOrigin` | CORS middleware via `$app->use()` |

---

## Service Layer

| Spring Boot | KislayPHP |
|---|---|
| `@Service` | Plain PHP class, no annotation needed |
| `@Autowired` | Manual injection (PHP constructor params) |
| `@Component` | N/A — PHP classes are lightweight |
| `@Transactional` | `DB::transaction(fn() => ...)` |
| `ApplicationContext` | N/A — no DI container by design |

---

## Data Layer

| Spring Boot | KislayPHP |
|---|---|
| Spring Data JPA / Hibernate | `Kislay\Persistence\DB` (PDO-based) |
| `EntityManager` | `DB::connection(name)` |
| `@Repository` | Plain PHP class using `DB::select/insert/update/delete` |
| `JpaRepository.findAll()` | `DB::select('SELECT * FROM ...')` |
| `@Entity` | Plain PHP class (no ORM annotations) |
| `Flyway / Liquibase` | `Kislay\Persistence\Migrations` |
| `spring.datasource.url` | `KISLAY_DB_DSN` env var |

---

## Configuration

| Spring Boot | KislayPHP |
|---|---|
| `@Value("${app.key}")` | `$config->get('app.key')` |
| `@ConfigurationProperties` | `$config->all()` → map to PHP class |
| `ConfigurableApplicationContext.refresh()` | `$config->refresh()` |
| `spring.config.import` | Multiple `setClient()` sources |
| Config server (Spring Cloud) | `Kislay\Config` with RPC client |

---

## Service Discovery & Load Balancing

| Spring Boot / Spring Cloud | KislayPHP |
|---|---|
| Eureka server | `Kislay\Discovery` (built-in) |
| `@EnableDiscoveryClient` | `$discovery->register('my-service', 'http://host:port')` |
| Ribbon / Spring Cloud LoadBalancer | `$discovery->resolve('service-name')` (weighted, round-robin, hash) |
| Heartbeat / lease renewal | `$discovery->heartbeat('name', 'url')` |
| Service health status | `$discovery->setStatus('name', 'url', 'DOWN')` |
| `@LoadBalanced RestTemplate` | `$discovery->resolve()` + PHP HTTP client |

---

## API Gateway

| Spring Boot / Spring Cloud | KislayPHP |
|---|---|
| Spring Cloud Gateway | `Kislay\Gateway` |
| Route predicates | `$gateway->proxy('/api/users', 'http://users-service')` |
| Filters (Auth, RateLimit) | `$gateway->setAuth()`, `$gateway->setRateLimit()` |
| Circuit Breaker (Resilience4j) | `$gateway->setCircuitBreaker(threshold, openSec)` |
| `spring.cloud.gateway.routes` | PHP route definitions |

---

## Messaging / Events

| Spring Boot | KislayPHP |
|---|---|
| `@EventListener` | `$server->on('event', fn)` |
| `ApplicationEventPublisher` | `$server->emit('event', $data)` |
| Spring AMQP / Kafka | `Kislay\Queue` with external client (Redis/RabbitMQ) |
| `@RabbitListener` | `$queue->subscribe('queue-name', fn)` |
| `@KafkaListener` | Same pattern |
| WebSocket STOMP | `Kislay\EventBus` WebSocket server |

---

## Observability

| Spring Boot | KislayPHP |
|---|---|
| Spring Actuator | Built into C++ extensions (access via `/metrics`) |
| Micrometer / Prometheus | `$metrics->exportPrometheus()` |
| Custom metrics | `$metrics->inc()`, `$metrics->gauge()`, `$metrics->observe()` |
| Distributed tracing | Phase 3 roadmap (W3C trace context) |
| Health endpoint | `$gateway->isRunning()`, `DB::ping()` |
| `management.endpoints.web.exposure` | Actuator endpoints via Gateway routing |

---

## Security

| Spring Boot | KislayPHP |
|---|---|
| Spring Security | `core-jwt-security` (Phase 2 — in progress) |
| JWT validation | `$app->setOption('jwt_secret', '...')` |
| `@PreAuthorize` | `$req->hasRole('admin')` |
| `UserDetails` | `$req->user()` (injected by JWT middleware) |
| Filter chain | C++ hot-path validation (no PHP overhead) |
| CORS | CORS middleware in `$app->use()` |

---

## Scheduling

| Spring Boot | KislayPHP |
|---|---|
| `@Scheduled(cron = "*/5 * * * * *")` | `$app->schedule('*/5 * * * *', fn)` (Phase 2) |
| `@Scheduled(fixedRate = 5000)` | `$app->every(5000, fn)` (Phase 2) |
| `TaskScheduler` | C++ timer thread pool |
| `@Async` | `$app->once(delayMs, fn)` (Phase 2) |

---

## Key Differences from Spring Boot

1. **No annotation processing** — KislayPHP uses PHP closures and arrays instead of annotations
2. **No DI container** — inject dependencies manually via PHP constructor params (intentional)
3. **C++ hot path** — auth, routing, connection pooling happen in C++ (Spring does this in JVM)
4. **Horizontal scale** — each PHP worker is a self-contained process with C++ extensions
5. **Memory** — PHP process ~10-20MB vs Spring Boot JVM ~200-500MB
6. **Startup** — KislayPHP starts in <100ms vs Spring Boot 5-30s
7. **Ecosystem** — Spring has 20 years of ecosystem; KislayPHP targets the PHP-native stack

---

## Migration Checklist

For a typical Spring Boot REST microservice:
- [ ] Replace `@RestController` methods → PHP closures or `[Class, 'method']` routes
- [ ] Replace Spring Data → `DB::select/insert/update/delete` or Eloquent
- [ ] Replace Flyway → `Migrations::add()->run()`
- [ ] Replace Eureka client → `$discovery->register()`
- [ ] Replace `@Value` → `$config->get()`
- [ ] Replace Spring Security JWT → `$app->setOption('jwt_secret', ...)` (Phase 2)
- [ ] Replace Spring Actuator → `$metrics->exportPrometheus()`
- [ ] Replace `@Scheduled` → `$app->schedule()` (Phase 2)
