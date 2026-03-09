# KislayPHP HTTP Extension - Technical Reference

**Version**: 0.0.2+  
**Target**: PHP 8.2+ (NTS & ZTS)  
**Performance**: 63,000+ req/sec  
**Status**: Production Ready

---

## 1. Architecture

### Thread Model Overview

KislayPHP uses **embedded Civetweb** C library for HTTP serving, integrated into PHP with a multi-threaded architecture:

```
┌─ PHP CLI Process (Main Thread)
│
├─ Civetweb HTTP Server (Embedded C)
│  ├─ HTTP Worker Thread 1
│  ├─ HTTP Worker Thread 2
│  ├─ HTTP Worker Thread 3
│  └─ HTTP Worker Thread N (configurable via num_threads)
│
└─ Async Event Loop (if async_enabled=true)
   ├─ IO Manager Thread (task queue, libcurl multi)
   └─ Async Worker Threads (configurable via async_threads)
```

#### Thread Safety Behavior

**NTS PHP (Non-Thread-Safe)**:
- Single global `kislay_php_mutex` protects entire Zend Engine
- All HTTP threads must acquire this lock before executing PHP code
- Result: **Only 1 concurrent request can execute** (serialized)
- Multiple threads exist, but mutex prevents parallel execution
- Use case: Development, single-core systems

**ZTS PHP (Thread-Safe)**:
- Each thread has its own PHP context (TSRMLS per-thread storage)
- Multiple requests execute **truly concurrently**
- Significant performance improvement on multi-core
- **Recommended for production deployment**
- Use case: High-performance production servers

```php
// Thread configuration example
$app = new Kislay\Core\App();
$app->setOption('threads', 8);           // HTTP worker threads
$app->setOption('async_threads', 4);     // Async worker threads
$app->listen('0.0.0.0', 8080);
```

### PHP Request Lifecycle

Each HTTP request follows this complete lifecycle:

```
1. HTTP connection arrives on socket (Civetweb C layer)
2. Civetweb allocates worker thread from pool
3. Worker thread calls: kislay_request_handler(mg_connection *conn)

4. CREATE SESSION
   └─ NTS: Acquire kislay_php_mutex (serialization point)
   ├─ NTS/ZTS: Call php_request_startup() (PHP engine init)
   ├─ NTS/ZTS: Create global zval for $GLOBALS

5. PARSE REQUEST
   └─ Extract HTTP method, path, headers, body
   └─ Parse route parameters (e.g., :id from /users/:id)
   └─ Create Kislay\Core\Request object
   └─ Create Kislay\Core\Response object
   └─ Generate traceId/spanId for W3C distributed tracing

6. EXECUTE onRequestStart hooks
   └─ Hooks registered via $app->onRequestStart(callable)

7. EXECUTE MIDDLEWARE CHAIN
   ├─ Global middleware ($app->use(callable)) - all requests
   ├─ Path-scoped middleware (from $app->group() if prefix matches)
   ├─ Route-specific middleware (if route handler provided middleware)
   └─ Route handler callable

8. ERROR HANDLING (if exception thrown)
   └─ Dispatch to error middleware (4+ parameter signature)
   └─ If no error middleware, call $app->onError() handler
   └─ If still unhandled, send 500 Internal Server Error

9. EXECUTE onRequestEnd hooks
   └─ Hooks registered via $app->onRequestEnd(callable)
   └─ These run AFTER response body sent (cleanup only)

10. RESPONSE SENT via Civetweb
    └─ HTTP status + headers + body transmitted to client

11. GARBAGE COLLECTION (if enabled)
    └─ Optional per-request GC flush (memory management)

12. REQUEST SHUTDOWN
    └─ Destructor: ~KislayPHPSession()
    ├─ Call php_request_shutdown() (PHP engine cleanup)
    ├─ NTS: Release kislay_php_mutex
    ├─ ZTS: Clean TSRMLS context
    └─ Return worker thread to pool
```

### Memory Management

**Per-Request Memory**:
- Each request has isolated memory scope via PHP request startup/shutdown
- Memory is freed automatically after response sent
- No memory leaks between requests

**Configuration**:
```php
// Set memory limit for this process
$app->setMemoryLimit(256 * 1024 * 1024);  // 256MB

// Check current usage
$used = memory_get_usage(true);
echo "Used: " . ($used / 1024 / 1024) . " MB";

// Enable garbage collection after each request
$app->enableGc(true);  // php.ini: display_errors=0 recommended
```

**Async Memory**:
- Async tasks run in separate PHP sessions (own memory context)
- Promises and callbacks maintain reference until settled
- Use `->finally()` to cleanup resources explicitly

```php
$app->get('/task', function ($req, $res) {
    $largeData = array_fill(0, 1000000, 'x');
    
    async(function () use ($largeData) {
        // Runs in separate session
        return sizeof($largeData);
    })->finally(function () use (&$largeData) {
        // Explicit cleanup
        unset($largeData);
    });
    
    $res->json(['status' => 'processing']);
});
```

---

## 2. Configuration Reference

All configuration is set via `$app->setOption()` **before** calling `listen()` or `listenAsync()`.

### Threading

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `num_threads` | int | `1` or `ENV[KISLAYPHP_THREADS]` | HTTP worker thread pool size. NTS PHP auto-clamps to 1. ZTS PHP: use 2-8x CPU cores. |
| `async_threads` | int | `1` or `ENV[KISLAYPHP_ASYNC_THREADS]` | Async task worker thread count. Recommended: 1-4 for most apps. |

```php
$app->setOption('num_threads', 8);        // 8 HTTP workers
$app->setOption('async_threads', 4);      // 4 async workers
```

### Timeouts & Request Limits

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `request_timeout_ms` | long | `10000` (10s) or `ENV` | Socket read timeout. If client doesn't send data within this time, connection closed. |
| `max_request_size` | long | `1048576` (1MB) or `ENV` | Maximum request body size in bytes. Larger requests rejected with 413 Payload Too Large. |

```php
$app->setOption('request_timeout_ms', 30000);   // 30s timeout
$app->setOption('max_request_size', 50 * 1024 * 1024);  // 50MB upload
```

### Security & CORS

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `cors` | bool | `false` or `ENV` | Enable CORS headers (`Access-Control-Allow-Origin: *`). Use for public APIs. |
| `referrer_policy` | string | `strict-origin-when-cross-origin` | HTTP `Referrer-Policy` header value. Options: `no-referrer`, `same-origin`, `strict-origin-when-cross-origin`. |
| `trust_proxy` | bool | `false` | Extract real client IP from `X-Forwarded-For` header (for reverse proxy). |
| `trusted_proxies` | string\|array | empty | Comma-separated IPs or `*` for all proxies allowed. Use with `trust_proxy=true`. |

```php
$app->setOption('cors', true);                  // Public API CORS
$app->setOption('referrer_policy', 'same-origin');
$app->setOption('trust_proxy', true);
$app->setOption('trusted_proxies', '*');        // Behind load balancer
```

### JWT Authentication

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `jwt_secret` | string | empty | Secret key for JWT validation. Non-empty = enables JWT middleware. |
| `jwt_required` | bool | `false` | Require JWT on all requests (except excluded paths). |
| `jwt_exclude` | array\|string | empty | Paths to exclude from JWT check. Comma-separated string or array. |

```php
$app->setOption('jwt_secret', 'super-secret-key-min-32-chars!!!');
$app->setOption('jwt_required', true);
$app->setOption('jwt_exclude', ['/login', '/register', '/health']);

// Or comma-separated string
$app->setOption('jwt_exclude', '/login,/register,/health');
```

### TLS/HTTPS

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tls_cert` | string | `ENV[KISLAY_CERT]` or `.env` | Path to SSL certificate (PEM format). Required for HTTPS. |
| `tls_key` | string | `ENV[KISLAY_KEY]` or `.env` | Path to SSL private key (PEM format). Required for HTTPS. |

```php
$app->setOption('tls_cert', '/etc/ssl/certs/server.crt');
$app->setOption('tls_key', '/etc/ssl/private/server.key');
$app->listen('0.0.0.0', 8443);  // HTTPS on 8443
```

### Features & Logging

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `log` | bool | `false` or `ENV` | Enable request logging to stderr. Format: `[METHOD] [PATH] [STATUS] [MS]ms`. |
| `async` | bool | `true` | Enable async event loop. Set to `false` for simple blocking-only servers. |
| `document_root` | string | empty | Static file serving root directory. If set, GET requests with no matching route serve files from this directory. |
| `actuator` | bool | `false` | Enable health/metrics endpoints (`/actuator/health`, `/actuator/metrics`). |

```php
$app->setOption('log', true);
$app->setOption('async', true);
$app->setOption('document_root', __DIR__ . '/public');
$app->setOption('actuator', true);  // ← http://localhost:8080/actuator/health
```

---

## 3. API Reference

### Request Object

**HTTP Metadata**:

```php
// Get HTTP method
$method = $request->method();              // "GET", "POST", etc.

// Get request path
$path = $request->path();                  // "/users/123"

// Get full URI
$uri = $request->uri();                    // "/users/123?sort=name"

// Get route parameters (from :id, :slug, etc.)
$userId = $request->param('id');           // "123"
$params = $request->getParams();           // ['id' => '123']
```

**Query String**:

```php
// Get query parameter
$sort = $request->query('sort');           // "name"
$sort = $request->query('sort', 'id');     // default to 'id'

// Get all query parameters as array
$all = $request->getQueryParams();         // ['sort' => 'name', 'page' => '1']
```

**Headers**:

```php
// Get single header
$auth = $request->header('Authorization');  // "Bearer TOKEN"

// Check header exists
if ($request->hasHeader('X-Custom')) { }

// Get all headers as array
$headers = $request->getHeaders();  // ['authorization' => 'Bearer ...', ...]
```

**Request Body**:

```php
// Get parsed body (auto-JSON decode if Content-Type: application/json)
$data = $request->body();                  // ['name' => 'John', 'email' => '...']

// Get as JSON
$json = $request->json();                  // same as body() for JSON

// Get single input field
$name = $request->input('name');           // "John"
$name = $request->input('name', 'Guest');  // default value

// Check field exists
if ($request->has('email')) { }

// Get only specified fields
$data = $request->only(['name', 'email']);  // filters to these keys

// Get all fields
$all = $request->all();                     // all body fields
```

**Context & Metadata**:

```php
// Get unique request ID (for logging/tracing)
$id = $request->id();                      // UUID-like string

// Get/set custom attributes (middleware → handler communication)
$request->setAttribute('user_id', 123);
$userId = $request->getAttribute('user_id');
$has = $request->hasAttribute('user_id');
```

**Distributed Tracing (W3C Trace Context)**:

```php
// Get trace ID (32-char hex, unique per request flow)
$traceId = $request->traceId();            // "4bf92f3577b34da6a3ce929d0e0e4736"

// Get span ID (16-char hex, unique per request)
$spanId = $request->spanId();              // "00f067aa0ba902b7"

// Get full W3C traceparent header
$traceparent = $request->traceparent();    // "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"

// Get tracestate (if forwarded)
$tracestate = $request->tracestate();      // "abc=123,def=456"
```

**Authentication**:

```php
// Get authenticated user (from JWT payload)
$user = $request->user();                  // decoded JWT payload object

// Check if user has role
$isAdmin = $request->hasRole('admin');     // true/false
```

### Response Object (Chainable)

**Status & Headers** (return `$this`):

```php
$response
    ->status(200)                          // HTTP status code
    ->header('X-Custom', 'value')          // Set header
    ->type('application/json');            // Set Content-Type
```

**Sending Responses**:

```php
// Send plain text
$response->send('Hello World', 200);

// Send JSON
$response->json(['message' => 'OK', 'data' => ['id' => 1]], 200);

// Send XML
$response->xml('<root><item>value</item></root>', 200);

// Stream file
$response->sendFile('/path/to/file.pdf');

// End with no body (e.g., 204 No Content)
$response->end();
```

**Status Helper Methods** (set status + send):

```php
// Success responses
$response->ok(['data' => 123]);             // 200 + JSON
$response->created(['id' => 1]);            // 201 + JSON
$response->noContent();                     // 204 + no body

// Client error responses
$response->badRequest(['error' => 'Invalid']);       // 400
$response->unauthorized(['error' => 'Auth failed']); // 401
$response->forbidden(['error' => 'Not allowed']);    // 403
$response->notFound(['error' => 'Not found']);       // 404
$response->methodNotAllowed();                       // 405
$response->conflict(['error' => 'Duplicate']);       // 409
$response->unprocessableEntity(['error' => '...']); // 422

// Server error responses
$response->internalServerError(['error' => '...']);  // 500
```

**State Inspection**:

```php
$body = $response->getBody();              // Current response body
$code = $response->getStatusCode();        // Current status code
```

### App Object

**Lifecycle**:

```php
$app = new Kislay\Core\App();

// Start blocking (thread sleeps until stop called)
$app->listen('0.0.0.0', 8080);             // Blocks forever

// Start non-blocking (returns immediately)
$app->listenAsync('0.0.0.0', 8080);
// ... do other work ...
$app->wait(5000);                          // Wait 5s for server startup

// Check status
$running = $app->isRunning();              // true/false

// Graceful shutdown
$app->stop();
```

**Configuration**:

```php
$app->setOption('threads', 4);
$app->setOption('jwt_secret', 'key');
// Set ALL options BEFORE listen/listenAsync
```

**Routing**:

```php
// HTTP method routes (chainable, no return value)
$app->get('/path', function ($req, $res) { });
$app->post('/path', function ($req, $res) { });
$app->put('/path', function ($req, $res) { });
$app->patch('/path', function ($req, $res) { });
$app->delete('/path', function ($req, $res) { });
$app->options('/path', function ($req, $res) { });

// Match any HTTP method
$app->all('/path', function ($req, $res) { });

// Generic method routing
$app->route('POST', '/path', function ($req, $res) { });

// Route parameters
$app->get('/users/:id', function ($req, $res) {
    $id = $req->param('id');
});

// Regex route patterns
$app->get('/files/:filename{.*}', function ($req, $res) {
    $filename = $req->param('filename');  // includes extension
});
```

**Middleware**:

```php
// Global middleware (all requests)
$app->use(function ($req, $res, $next) {
    error_log("Before: " . $req->method() . " " . $req->path());
    $next();
    error_log("After");
});

// Path-scoped middleware
$app->use('/api', function ($req, $res, $next) {
    $res->header('Content-Type', 'application/json');
    $next();
});

// Route grouping with shared prefix + middleware
$app->group('/api/v1', function () use ($app) {
    $app->use(function ($req, $res, $next) {
        if (!$req->hasHeader('X-API-Key')) {
            $res->unauthorized(['error' => 'Missing API key']);
            return;
        }
        $next();
    });

    $app->get('/users', function ($req, $res) {
        $res->json(['users' => []]);
    });
});
```

**Lifecycle Hooks**:

```php
// Called at start of each request (before middleware)
$app->onRequestStart(function ($req) {
    error_log("Request start: " . $req->id());
});

// Called at end of each request (after response sent)
$app->onRequestEnd(function ($req) {
    error_log("Request end: " . $req->id());
});

// Called for 404 Not Found
$app->onNotFound(function ($req, $res) {
    $res->notFound(['error' => 'Route not found']);
});

// Called if route handler throws exception
$app->onError(function ($error, $req, $res) {
    error_log("Error: " . $error->getMessage());
    $res->internalServerError(['error' => 'Server error']);
});
```

**Task Scheduling**:

```php
// Recurring task (every N milliseconds)
$app->every(60000, function () {
    error_log("Running every 60 seconds");
});

// One-time delayed task
$app->once(30000, function () {
    error_log("Running after 30 seconds delay");
});

// Cron-style scheduling
$app->schedule('0 * * * *', function () {
    error_log("Running at top of every hour");
});
```

---

## 4. Patterns and Recipes

### RESTful CRUD API

```php
<?php
$app = new Kislay\Core\App();
$app->setOption('threads', 4);

// In-memory database (replace with real DB)
$users = [
    1 => ['id' => 1, 'name' => 'Alice', 'email' => 'alice@example.com'],
];
$nextId = 2;

// CREATE
$app->post('/users', function ($req, $res) use (&$users, &$nextId) {
    $data = $req->body();
    
    if (!isset($data['name']) || !isset($data['email'])) {
        $res->badRequest(['error' => 'Missing name or email']);
        return;
    }
    
    $user = [
        'id' => $nextId++,
        'name' => $data['name'],
        'email' => $data['email']
    ];
    $users[$user['id']] = $user;
    
    $res->status(201)->json(['data' => $user]);
});

// READ (list all)
$app->get('/users', function ($req, $res) use ($users) {
    $res->json(['data' => array_values($users)]);
});

// READ (single)
$app->get('/users/:id', function ($req, $res) use ($users) {
    $id = (int)$req->param('id');
    
    if (!isset($users[$id])) {
        $res->notFound(['error' => 'User not found']);
        return;
    }
    
    $res->json(['data' => $users[$id]]);
});

// UPDATE
$app->put('/users/:id', function ($req, $res) use (&$users) {
    $id = (int)$req->param('id');
    
    if (!isset($users[$id])) {
        $res->notFound(['error' => 'User not found']);
        return;
    }
    
    $data = $req->body();
    if (isset($data['name'])) $users[$id]['name'] = $data['name'];
    if (isset($data['email'])) $users[$id]['email'] = $data['email'];
    
    $res->json(['data' => $users[$id]]);
});

// DELETE
$app->delete('/users/:id', function ($req, $res) use (&$users) {
    $id = (int)$req->param('id');
    
    if (!isset($users[$id])) {
        $res->notFound(['error' => 'User not found']);
        return;
    }
    
    unset($users[$id]);
    $res->noContent();
});

$app->listen('0.0.0.0', 8080);
```

### File Upload Handler

```php
<?php
$app = new Kislay\Core\App();
$uploadDir = __DIR__ . '/uploads';
mkdir($uploadDir, 0755, true);

$app->post('/upload', function ($req, $res) use ($uploadDir) {
    $contentType = $req->header('Content-Type') ?? '';
    
    if (strpos($contentType, 'multipart/form-data') === false) {
        $res->badRequest(['error' => 'Content-Type must be multipart/form-data']);
        return;
    }
    
    // Get file from request
    $body = $req->body();
    
    if (!isset($body['file'])) {
        $res->badRequest(['error' => 'No file provided']);
        return;
    }
    
    $fileData = $body['file'];
    $originalName = $fileData['name'] ?? 'file';
    $content = $fileData['content'] ?? '';
    
    // Sanitize filename
    $filename = uniqid() . '_' . preg_replace('/[^a-zA-Z0-9._-]/', '', $originalName);
    $filepath = $uploadDir . '/' . $filename;
    
    if (!file_put_contents($filepath, $content)) {
        $res->internalServerError(['error' => 'Failed to save file']);
        return;
    }
    
    $res->status(201)->json([
        'data' => [
            'filename' => $filename,
            'size' => strlen($content),
            'url' => '/files/' . $filename
        ]
    ]);
});

// Serve uploaded files
$app->get('/files/:filename{.*}', function ($req, $res) use ($uploadDir) {
    $filename = $req->param('filename');
    $filepath = $uploadDir . '/' . basename($filename);  // Security: no path traversal
    
    if (!file_exists($filepath) || !is_file($filepath)) {
        $res->notFound(['error' => 'File not found']);
        return;
    }
    
    $res->sendFile($filepath);
});

$app->listen('0.0.0.0', 8080);
```

### JWT Authentication & Authorization

```php
<?php
$app = new Kislay\Core\App();

// Set JWT secret (enables JWT validation middleware)
$app->setOption('jwt_secret', 'super-secret-key-at-least-32-chars');
$app->setOption('jwt_required', true);

// Paths that bypass JWT (e.g., login endpoint)
$app->setOption('jwt_exclude', ['/login', '/health', '/docs']);

// Login endpoint (generates JWT)
$app->post('/login', function ($req, $res) {
    $credentials = $req->body();
    
    // Validate credentials (replace with real auth)
    if ($credentials['username'] !== 'admin' || $credentials['password'] !== 'password123') {
        $res->unauthorized(['error' => 'Invalid credentials']);
        return;
    }
    
    // Create JWT payload
    $payload = [
        'user_id' => 1,
        'username' => 'admin',
        'roles' => ['admin', 'user'],
        'iat' => time(),
        'exp' => time() + 3600  // 1 hour expiry
    ];
    
    // Generate JWT (internal: handled by KislayPHP with jwt_secret)
    // Response must include token for client
    $res->json([
        'token' => 'eyJ...',  // In real code, use JWT library
        'expires_in' => 3600
    ]);
});

// Protected endpoint (JWT required by default)
$app->get('/profile', function ($req, $res) {
    // User already validated by JWT middleware
    $user = $req->user();
    
    $res->json([
        'data' => [
            'id' => $user['user_id'],
            'username' => $user['username'],
            'roles' => $user['roles']
        ]
    ]);
});

// Role-based access
$app->delete('/users/:id', function ($req, $res) {
    $user = $req->user();
    
    if (!$req->hasRole('admin')) {
        $res->forbidden(['error' => 'Admin role required']);
        return;
    }
    
    // Delete user...
    $res->noContent();
});

// Health check (excluded from JWT via jwt_exclude)
$app->get('/health', function ($req, $res) {
    $res->ok(['status' => 'healthy']);
});

$app->listen('0.0.0.0', 8080);
```

### Middleware for Logging

```php
<?php
$app = new Kislay\Core\App();
$app->setOption('log', true);  // Automatic request logging

// Custom middleware for detailed logging
$app->use(function ($req, $res, $next) {
    $startTime = microtime(true);
    $traceId = $req->traceId();
    
    error_log("[$traceId] START {$req->method()} {$req->path()}");
    
    if ($req->hasHeader('Authorization')) {
        error_log("[$traceId] Auth: " . substr($req->header('Authorization'), 0, 20) . "...");
    }
    
    // Execute route handler
    $next();
    
    // Log after response
    $elapsed = (microtime(true) - $startTime) * 1000;
    $status = $res->getStatusCode();
    error_log("[$traceId] END status=$status elapsed=${elapsed}ms");
});

// Error logging middleware
$app->use(function ($req, $res, $next) {
    try {
        $next();
    } catch (\Throwable $e) {
        $traceId = $req->traceId();
        error_log("[$traceId] ERROR: " . $e->getMessage() . " in " . $e->getFile() . ":" . $e->getLine());
        
        $res->status(500)->json([
            'error' => 'Internal Server Error',
            'trace_id' => $traceId
        ]);
    }
});

$app->get('/users', function ($req, $res) {
    $res->json(['users' => []]);
});

$app->listen('0.0.0.0', 8080);
```

### Error Handling & Recovery

```php
<?php
$app = new Kislay\Core\App();

// Error middleware (auto-detected by 4+ parameters)
$app->use(function ($error, $req, $res, $next) {
    if ($error instanceof \InvalidArgumentException) {
        $res->badRequest(['error' => $error->getMessage()]);
        return;  // Stop error chain (don't call $next)
    }
    
    if ($error instanceof \RuntimeException) {
        $res->internalServerError(['error' => 'Service temporarily unavailable']);
        return;
    }
    
    // Pass to next error handler
    $next();
});

// Global error handler (fallback)
$app->onError(function ($error, $req, $res) {
    error_log("Unhandled error: " . $error->getMessage());
    $res->internalServerError(['error' => 'Internal Server Error']);
});

$app->get('/validate/:id', function ($req, $res) {
    $id = (int)$req->param('id');
    
    if ($id <= 0) {
        throw new \InvalidArgumentException('ID must be positive');
    }
    
    $res->ok(['valid' => true]);
});

$app->listen('0.0.0.0', 8080);
```

---

## 5. Performance Notes

### Threading Strategy

**HTTP Thread Pool**:
```php
// Development (single thread)
$app->setOption('threads', 1);

// Production (2-4x CPU cores)
$numCores = shell_exec('nproc');
$app->setOption('threads', $numCores * 2);  // Common: 8-16 threads on 4-8 core system

// High-concurrency (e.g., proxy/gateway)
$app->setOption('threads', 64);  // Thousands of connections possible
```

**Async Worker Threads**:
```php
// For async HTTP requests or background tasks
$app->setOption('async_threads', 4);  // 1-4 typically sufficient

// More threads = more concurrent async tasks
$app->setOption('async_threads', 16);  // For very async-heavy apps
```

### Memory Optimization

**Per-Request Memory**:
- Each request auto-frees memory after response
- Typical request: 1-5 MB (PHP + extensions)
- With large payloads: +size of request/response bodies

```php
// Monitor memory usage
$app->get('/memory', function ($req, $res) {
    $res->json([
        'used_mb' => round(memory_get_usage(true) / 1024 / 1024, 2),
        'peak_mb' => round(memory_get_peak_usage(true) / 1024 / 1024, 2)
    ]);
});
```

**Garbage Collection**:
```php
// Enable GC between requests (can slow down slightly but reduces memory)
$app->enableGc(true);

// Memory limit
$app->setMemoryLimit(512 * 1024 * 1024);  // 512 MB
```

### Timeout Configuration

```php
// Request timeout (max time to read request from socket)
$app->setOption('request_timeout_ms', 30000);  // 30s for slow uploads

// Handler timeout (implement in middleware if needed)
$app->use(function ($req, $res, $next) {
    $startTime = microtime(true);
    
    // Set max execution time
    set_time_limit(60);  // 60s max handler execution
    
    $next();
});
```

### Performance Benchmarks

**Typical Performance** (on modern 4-core CPU):
- **Simple GET (no DB)**: 8,000-12,000 req/sec
- **JSON parsing + response**: 5,000-8,000 req/sec
- **Database query (Redis)**: 2,000-3,000 req/sec
- **File upload (1MB)**: 500-1,000 req/sec

**Factors Affecting Performance**:
1. NTS vs ZTS: ZTS 2-4x faster due to parallel execution
2. Thread pool size: Optimal at 2-4x CPU cores
3. Middleware count: Each middleware adds ~50-100 microseconds
4. JSON/request parsing: Large payloads impact directly
5. Database latency: External service bottleneck

---

## 6. Troubleshooting

### Issue 1: "Too many open files" Error

**Symptom**: `bind(): Address already in use` or high concurrency connections fail

**Causes**:
- System file descriptor limit too low
- Port not releasing after shutdown

**Solution**:
```bash
# Check current limit
ulimit -n

# Increase limit (permanent in /etc/security/limits.conf)
# Add: * soft nofile 65536
#      * hard nofile 65536

# Restart session, verify
ulimit -n
```

```php
$app->setOption('threads', 4);  // Don't exceed safe limits
$app->listen('0.0.0.0', 8080);

// Wait for port to release before restart
sleep(2);
```

### Issue 2: Requests Serializing (NTS PHP Only)

**Symptom**: High concurrency, but only 1 request executing at a time (others wait)

**Causes**:
- Using NTS (Non-Thread-Safe) PHP build
- Single global mutex serializes all PHP execution

**Solution**:
```bash
# Check your PHP build
php -v | grep -E '(NTS|ZTS)'

# If NTS, rebuild PHP with --enable-zts
./configure --prefix=/usr/local/php --enable-zts
make && make install

# Use ZTS in KislayPHP
$app->setOption('threads', 8);  # Now true concurrency!
```

### Issue 3: Memory Leaks / Growing Process

**Symptom**: Process memory grows over time, never decreases

**Causes**:
- Unfreed references in middleware/handlers
- Circular object references
- Large data stored in static variables

**Solution**:
```php
// Enable GC between requests
$app->enableGc(true);

// Explicitly free resources
$app->use(function ($req, $res, $next) {
    $largeData = fetchLargeData();
    
    $next();
    
    // Cleanup after response
    unset($largeData);
});

// Monitor with memory endpoint
$app->get('/debug/memory', function ($req, $res) {
    $res->json([
        'current_mb' => memory_get_usage(true) / 1024 / 1024,
        'peak_mb' => memory_get_peak_usage(true) / 1024 / 1024
    ]);
});
```

### Issue 4: JWT Not Validating

**Symptom**: 401 Unauthorized on protected routes even with valid token

**Causes**:
- JWT secret not set or mismatched
- Token expired
- Token malformed or incomplete

**Solution**:
```php
// Verify secret is set correctly
$app->setOption('jwt_secret', 'your-exact-secret-key');

// Check excluded paths
$app->setOption('jwt_exclude', ['/login', '/health']);

// Debug token
$app->get('/debug/token', function ($req, $res) {
    $auth = $req->header('Authorization') ?? 'none';
    $user = $req->user() ?? null;
    
    $res->json([
        'auth_header' => substr($auth, 0, 30) . '...',
        'user' => $user,
        'has_role_admin' => $user ? $req->hasRole('admin') : false
    ]);
});
```

### Issue 5: CORS Requests Failing

**Symptom**: Preflight OPTIONS requests rejected or `Access-Control-Allow-Origin` missing

**Causes**:
- CORS not enabled
- Incorrect origin in response headers

**Solution**:
```php
$app->setOption('cors', true);  // Enable CORS

// Manual CORS middleware if finer control needed
$app->use(function ($req, $res, $next) {
    $origin = $req->header('Origin') ?? '*';
    
    $res->header('Access-Control-Allow-Origin', $origin);
    $res->header('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS');
    $res->header('Access-Control-Allow-Headers', 'Content-Type,Authorization');
    $res->header('Access-Control-Max-Age', '86400');
    
    if ($req->method() === 'OPTIONS') {
        $res->end();
        return;
    }
    
    $next();
});
```

### Issue 6: 413 Payload Too Large

**Symptom**: File uploads fail with 413 status

**Causes**:
- Request body exceeds `max_request_size`

**Solution**:
```php
// Increase limit (default 1MB)
$app->setOption('max_request_size', 100 * 1024 * 1024);  // 100MB

// Validate file size in handler
$app->post('/upload', function ($req, $res) {
    $size = strlen($req->body());
    if ($size > 50 * 1024 * 1024) {
        $res->status(413)->json(['error' => 'File too large']);
        return;
    }
    // Process file...
});
```

### Issue 7: Async Tasks Not Running

**Symptom**: `async()` or `executeAsync()` callbacks never fire

**Causes**:
- Async disabled (`async_enabled=false`)
- Handler exits before async completes
- Promise callbacks forgotten

**Solution**:
```php
$app->setOption('async', true);  // Ensure enabled (default)

// Hold server alive for async
$app->get('/task', function ($req, $res) {
    async(function () {
        sleep(2);
        error_log("Task complete");
    })->then(function () {
        error_log("Promise resolved");
    });
    
    // Important: Return response to prevent premature shutdown
    $res->json(['status' => 'processing']);
});

// Keep listening (blocks)
$app->listen('0.0.0.0', 8080);
```

### Issue 8: Timeout During Long Operations

**Symptom**: 500 error or connection reset on slow requests

**Causes**:
- `request_timeout_ms` too short
- Handler exceeds `set_time_limit()`
- Database query hanging

**Solution**:
```php
// Increase request timeout
$app->setOption('request_timeout_ms', 120000);  // 120s

// Increase handler timeout
ini_set('max_execution_time', 300);  // 5 minutes

// Break long operations into chunks
$app->post('/process-large', function ($req, $res) {
    set_time_limit(600);  // 10 minutes for this request
    
    $data = $req->body();
    $items = $data['items'] ?? [];
    
    $results = [];
    foreach ($items as $i => $item) {
        $results[] = processItem($item);
        
        // Yield control periodically
        if ($i % 100 === 0) {
            usleep(1000);  // 1ms
        }
    }
    
    $res->json(['processed' => count($results)]);
});
```

### Issue 9: High CPU Usage Under Load

**Symptom**: CPU maxes out, latency increases

**Causes**:
- Thread pool size too large (context switching overhead)
- Busy-waiting in handlers
- Unoptimized database queries

**Solution**:
```php
// Optimal thread count (2-4x CPU cores, not more)
$numCores = shell_exec('nproc');
$app->setOption('threads', $numCores * 2);  // Not 64!

// Add request queuing middleware
$app->use(function ($req, $res, $next) {
    // Example: reject requests if queue too long
    if (function_exists('os_getloadavg')) {
        $load = os_getloadavg();
        if ($load[0] > 10) {  // Load average high
            $res->status(503)->json(['error' => 'Server overloaded']);
            return;
        }
    }
    $next();
});

// Profile slow routes
$app->use(function ($req, $res, $next) {
    $start = microtime(true);
    $next();
    $elapsed = (microtime(true) - $start) * 1000;
    
    if ($elapsed > 100) {  // Log slow requests (>100ms)
        error_log("SLOW: {$req->method()} {$req->path()} ${elapsed}ms");
    }
});
```

### Issue 10: 404 Not Found on Valid Routes

**Symptom**: Route exists but returns 404

**Causes**:
- Route pattern doesn't match request path
- Case-sensitivity mismatch
- Leading/trailing slashes

**Solution**:
```php
// Debug route matching
$app->get('/debug/routes', function ($req, $res) {
    $path = $req->query('path') ?? '/users/123';
    
    // Check manually
    error_log("Checking path: $path");
    
    // Ensure routes registered
    $app->get('/users/:id', function ($req, $res) {
        // This route won't appear here, but is registered above
    });
});

// Exact route matching
$app->get('/', function ($req, $res) {
    $res->ok(['message' => 'Root']);
});

// With parameters
$app->get('/users/:id', function ($req, $res) {
    $res->ok(['id' => $req->param('id')]);
});

// Regex/wildcard
$app->get('/files/:path{.*}', function ($req, $res) {
    $res->ok(['file' => $req->param('path')]);
});

// Fallback 404
$app->onNotFound(function ($req, $res) {
    error_log("404: {$req->method()} {$req->path()}");
    $res->notFound([
        'error' => 'Route not found',
        'path' => $req->path(),
        'method' => $req->method()
    ]);
});
```

---

## Summary

KislayPHP provides a **high-performance, embedded HTTP server for PHP microservices** with:

✅ Multi-threaded request handling (NTS serialized, ZTS parallel)  
✅ Flexible routing with path parameters and regex patterns  
✅ Middleware system with error handlers  
✅ Async tasks and HTTP client with promises  
✅ JWT authentication with role-based access  
✅ W3C Distributed Tracing for observability  
✅ TLS/HTTPS support  
✅ Comprehensive error handling  

For production deployments, use **ZTS PHP**, configure **2-4x CPU core threads**, enable **request logging**, and monitor **memory usage** regularly.
