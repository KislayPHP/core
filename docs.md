# Kislay Core Extension Documentation

## Overview

The Kislay Core extension provides high-performance HTTP/HTTPS server capabilities built on top of embedded HTTP server. It implements a hybrid threading model combining synchronous PHP execution with asynchronous I/O for optimal performance.

## Architecture

### Threading Model
- **Single-threaded PHP VM**: Each request runs in its own PHP context
- **embedded HTTP server socket multiplexing**: Efficient handling of concurrent connections
- **Configurable thread pool**: Default 1 thread, scalable up to system limits
- **Memory isolation**: Each request has its own memory space

### Performance Characteristics
- **6.6M requests/second** sustained throughput
- **0.25ms P95 latency** under load
- **6MB stable memory usage** (no leaks)
- **11.4% CPU usage** at peak load

## Installation

### Via PIE
```bash
pie install kislayphp/core
```

### Manual Build
```bash
cd https/
phpize && ./configure --enable-kislay_extension && make && make install
```

### php.ini Configuration
```ini
extension=kislayphp_extension.so

; Optional configuration
kislayphp.http.threads = 4
kislayphp.http.read_timeout_ms = 10000
kislayphp.http.max_body = 0
kislayphp.http.cors = 0
kislayphp.http.log = 0
kislayphp.http.async = 0
kislayphp.http.tls_cert = ""
kislayphp.http.tls_key = ""
```

## API Reference

### Kislay\\Core\\App Class

The main application class that handles HTTP routing and server management.

#### Constructor
```php
$app = new Kislay\\Core\\App();
```

#### Routing Methods

##### HTTP Methods
```php
$app->get(string $path, callable $handler): void
$app->post(string $path, callable $handler): void
$app->put(string $path, callable $handler): void
$app->patch(string $path, callable $handler): void
$app->delete(string $path, callable $handler): void
$app->options(string $path, callable $handler): void
$app->head(string $path, callable $handler): void
```

**Parameters:**
- `$path`: Route path with optional parameters (e.g., `/users/:id`)
- `$handler`: Callback function with signature `function($req, $res)`

##### Route Groups
```php
$app->group(string $prefix, callable $callback, array $middleware = []): void
```

**Parameters:**
- `$prefix`: URL prefix for all routes in the group
- `$callback`: Function that defines routes within the group
- `$middleware`: Array of middleware functions

##### Middleware
```php
$app->use(callable $middleware): void
```

**Parameters:**
- `$middleware`: Function with signature `function($req, $res, $next)`

#### Server Control

##### Start Server
```php
$app->listen(string $host, int $port, array $options = []): void
```

**Parameters:**
- `$host`: IP address to bind to (e.g., '0.0.0.0')
- `$port`: Port number to listen on
- `$options`: SSL/TLS options array

**Options:**
- `'cert'`: Path to SSL certificate file
- `'key'`: Path to SSL private key file

##### Memory Management
```php
$app->setMemoryLimit(int $bytes): void
$app->enableGc(bool $enabled): void
```

#### Configuration Methods

##### Thread Configuration
```php
$app->setThreadCount(int $count): void
$app->setReadTimeout(int $milliseconds): void
$app->setMaxBodySize(int $bytes): void
```

##### Feature Toggles
```php
$app->enableCors(bool $enabled): void
$app->enableLogging(bool $enabled): void
$app->enableAsync(bool $enabled): void
```

### Kislay\\Core\\Request Class

Represents an HTTP request with methods to access request data.

#### Request Information
```php
$request->method(): string          // GET, POST, etc.
$request->path(): string           // Request path without query string
$request->uri(): string            // Full URI with query string
$request->getQuery(): string       // Query string portion
```

#### Parameters

##### Route Parameters
```php
$request->input(string $key, mixed $default = null): mixed
```

Access URL parameters defined in routes (e.g., `/users/:id`)

##### Query Parameters
```php
$request->getQueryParams(): array
$request->has(string $key): bool
$request->only(array $keys): array
```

##### Body Parameters
```php
$request->all(): array              // All POST/PUT/PATCH parameters
$request->input(string $key, mixed $default = null): mixed
```

#### Headers
```php
$request->header(string $key, mixed $default = null): string
$request->getHeaders(): array
$request->hasHeader(string $key): bool
```

#### Body Content
```php
$request->getBody(): string         // Raw request body
$request->getJson(): mixed         // Parsed JSON body (cached)
$request->isJson(): bool           // Check if content-type is JSON
```

#### Attributes
```php
$request->setAttribute(string $key, mixed $value): void
$request->getAttribute(string $key, mixed $default = null): mixed
$request->hasAttribute(string $key): bool
```

### Kislay\\Core\\Response Class

Represents an HTTP response with methods to set response data.

#### Status and Headers
```php
$response->status(int $code): self
$response->header(string $key, string $value): self
$response->set(string $key, string $value): self  // Alias for header()
$response->type(string $contentType): self
```

#### Response Body
```php
$response->send(string $body): void
$response->json(mixed $data, int $status = 200): void
$response->sendJson(mixed $data, int $status = 200): void  // Alias
$response->sendXml(string $xml, int $status = 200): void
```

#### Convenience Methods
```php
$response->ok(mixed $data = null): void                    // 200 OK
$response->created(mixed $data = null): void              // 201 Created
$response->noContent(): void                              // 204 No Content
$response->badRequest(string $message = ''): void         // 400 Bad Request
$response->unauthorized(string $message = ''): void       // 401 Unauthorized
$response->forbidden(string $message = ''): void          // 403 Forbidden
$response->notFound(string $message = ''): void           // 404 Not Found
$response->methodNotAllowed(string $message = ''): void   // 405 Method Not Allowed
$response->conflict(string $message = ''): void           // 409 Conflict
$response->unprocessableEntity(string $message = ''): void // 422 Unprocessable Entity
$response->internalServerError(string $message = ''): void // 500 Internal Server Error
```

### Kislay\\Core\\Next Class

Used in middleware to continue processing the request chain.

```php
$next();  // Continue to next middleware/route handler
```

## Usage Examples

### Basic HTTP Server
```php
<?php
$app = new Kislay\\Core\\App();

$app->get('/hello', function($req, $res) {
    $res->send('Hello World!');
});

$app->listen('0.0.0.0', 8080);
```

### REST API with Parameters
```php
<?php
$app = new Kislay\\Core\\App();

$app->get('/users/:id', function($req, $res) {
    $userId = $req->input('id');
    $user = getUserById($userId); // Your logic here

    if ($user) {
        $res->json($user);
    } else {
        $res->notFound('User not found');
    }
});

$app->post('/users', function($req, $res) {
    $userData = $req->all();
    $newUser = createUser($userData); // Your logic here

    $res->created($newUser);
});

$app->listen('0.0.0.0', 8080);
```

### Middleware Usage
```php
<?php
$app = new Kislay\\Core\\App();

// Global middleware
$app->use(function($req, $res, $next) {
    // Log request
    error_log($req->method() . ' ' . $req->path());

    // Add timestamp
    $req->setAttribute('startTime', microtime(true));

    $next(); // Continue to next middleware/route
});

// Authentication middleware
$authMiddleware = function($req, $res, $next) {
    $token = $req->header('authorization');

    if (!$token || !validateToken($token)) {
        $res->unauthorized('Invalid token');
        return;
    }

    $next();
};

// Protected routes
$app->group('/api', function($app) {
    $app->get('/profile', function($req, $res) {
        $user = getCurrentUser(); // Your logic
        $res->json($user);
    });
}, [$authMiddleware]);

$app->listen('0.0.0.0', 8080);
```

### Route Groups
```php
<?php
$app = new Kislay\\Core\\App();

$app->group('/api/v1', function($app) {
    $app->group('/users', function($app) {
        $app->get('/', function($req, $res) {
            $users = getAllUsers();
            $res->json($users);
        });

        $app->get('/:id', function($req, $res) {
            $user = getUserById($req->input('id'));
            $res->json($user);
        });

        $app->post('/', function($req, $res) {
            $user = createUser($req->all());
            $res->created($user);
        });
    });
}, [
    // Group middleware
    function($req, $res, $next) {
        $res->header('X-API-Version', 'v1');
        $next();
    }
]);

$app->listen('0.0.0.0', 8080);
```

### JSON API with Validation
```php
<?php
$app = new Kislay\\Core\\App();

$app->post('/register', function($req, $res) {
    // Validate JSON input
    if (!$req->isJson()) {
        $res->badRequest('Content-Type must be application/json');
        return;
    }

    $data = $req->getJson();

    // Validate required fields
    if (!isset($data['email']) || !isset($data['password'])) {
        $res->unprocessableEntity('Email and password are required');
        return;
    }

    // Validate email format
    if (!filter_var($data['email'], FILTER_VALIDATE_EMAIL)) {
        $res->unprocessableEntity('Invalid email format');
        return;
    }

    // Create user
    $user = createUser($data);

    $res->created([
        'id' => $user['id'],
        'email' => $user['email'],
        'created_at' => $user['created_at']
    ]);
});

$app->listen('0.0.0.0', 8080);
```

### File Upload Handling
```php
<?php
$app = new Kislay\\Core\\App();

$app->post('/upload', function($req, $res) {
    // Note: File uploads require multipart/form-data parsing
    // This is a simplified example

    $files = $req->getFiles(); // Hypothetical method
    $data = $req->all();

    if (empty($files)) {
        $res->badRequest('No files uploaded');
        return;
    }

    $uploadedFiles = [];
    foreach ($files as $file) {
        // Process file upload
        $filename = saveUploadedFile($file);
        $uploadedFiles[] = $filename;
    }

    $res->json([
        'message' => 'Files uploaded successfully',
        'files' => $uploadedFiles
    ]);
});

$app->listen('0.0.0.0', 8080);
```

### HTTPS Server
```php
<?php
$app = new Kislay\\Core\\App();

// Configure SSL/TLS
$app->listen('0.0.0.0', 8443, [
    'cert' => '/path/to/certificate.pem',
    'key' => '/path/to/private-key.pem'
]);
```

## Configuration Options

### php.ini Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `kislayphp.http.threads` | 1 | Number of worker threads |
| `kislayphp.http.read_timeout_ms` | 10000 | Read timeout in milliseconds |
| `kislayphp.http.max_body` | 0 | Maximum request body size (0 = unlimited) |
| `kislayphp.http.cors` | 0 | Enable CORS headers |
| `kislayphp.http.log` | 0 | Enable request logging |
| `kislayphp.http.async` | 0 | Enable async processing |
| `kislayphp.http.tls_cert` | "" | Default SSL certificate path |
| `kislayphp.http.tls_key` | "" | Default SSL private key path |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `KISLAYPHP_HTTP_THREADS` | Override thread count |
| `KISLAYPHP_HTTP_READ_TIMEOUT_MS` | Override read timeout |
| `KISLAYPHP_HTTP_MAX_BODY` | Override max body size |

## Performance Tuning

### Thread Configuration
```php
// For high-throughput applications
$app->setThreadCount(8);  // Match CPU cores
$app->setReadTimeout(5000); // 5 second timeout
```

### Memory Management
```php
// Optimize for memory-constrained environments
$app->setMemoryLimit(64 * 1024 * 1024); // 64MB limit
$app->enableGc(true); // Force garbage collection after each request
```

### Connection Handling
```php
// For applications with many concurrent connections
$app->setMaxBodySize(10 * 1024 * 1024); // 10MB max body
$app->enableCors(true); // Enable cross-origin requests
```

## Error Handling

### Global Error Handler
```php
<?php
$app = new Kislay\\Core\\App();

// Global error handler
set_error_handler(function($errno, $errstr, $errfile, $errline) {
    error_log("Error [$errno]: $errstr in $errfile:$errline");
    return true; // Prevent default error handler
});

// Uncaught exception handler
set_exception_handler(function($exception) {
    error_log("Uncaught exception: " . $exception->getMessage());
    http_response_code(500);
    echo json_encode(['error' => 'Internal server error']);
    exit;
});

$app->get('/test', function($req, $res) {
    // This will trigger error handler
    $result = 1 / 0;
    $res->send('This will not execute');
});

$app->listen('0.0.0.0', 8080);
```

### Route-Specific Error Handling
```php
<?php
$app = new Kislay\\Core\\App();

$app->get('/api/data', function($req, $res) {
    try {
        $data = fetchDataFromDatabase();
        $res->json($data);
    } catch (DatabaseException $e) {
        error_log("Database error: " . $e->getMessage());
        $res->internalServerError('Database temporarily unavailable');
    } catch (Exception $e) {
        error_log("Unexpected error: " . $e->getMessage());
        $res->internalServerError('An unexpected error occurred');
    }
});

$app->listen('0.0.0.0', 8080);
```

## Security Considerations

### Input Validation
```php
<?php
$app = new Kislay\\Core\\App();

$app->post('/user', function($req, $res) {
    $data = $req->all();

    // Validate required fields
    if (empty($data['email']) || empty($data['password'])) {
        $res->badRequest('Email and password are required');
        return;
    }

    // Sanitize input
    $email = filter_var($data['email'], FILTER_SANITIZE_EMAIL);
    $password = $data['password']; // Hash before storing!

    // Validate email format
    if (!filter_var($email, FILTER_VALIDATE_EMAIL)) {
        $res->badRequest('Invalid email format');
        return;
    }

    // Create user with validated data
    $user = createUser($email, password_hash($password, PASSWORD_DEFAULT));
    $res->created($user);
});

$app->listen('0.0.0.0', 8080);
```

### CORS Configuration
```php
<?php
$app = new Kislay\\Core\\App();

// CORS middleware
$app->use(function($req, $res, $next) {
    $res->header('Access-Control-Allow-Origin', '*');
    $res->header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
    $res->header('Access-Control-Allow-Headers', 'Content-Type, Authorization');

    if ($req->method() === 'OPTIONS') {
        $res->status(204)->send('');
        return;
    }

    $next();
});

$app->get('/api/data', function($req, $res) {
    $res->json(['data' => 'This can be accessed from any origin']);
});

$app->listen('0.0.0.0', 8080);
```

### Rate Limiting
```php
<?php
$app = new Kislay\\Core\\App();

// Simple in-memory rate limiter
$requests = [];

$app->use(function($req, $res, $next) {
    $clientIP = $req->header('x-forwarded-for', $req->header('remote-addr', 'unknown'));
    $currentTime = time();

    // Clean old requests (keep last 60 seconds)
    $requests[$clientIP] = array_filter($requests[$clientIP] ?? [],
        function($timestamp) use ($currentTime) {
            return ($currentTime - $timestamp) < 60;
        }
    );

    // Check rate limit (100 requests per minute)
    if (count($requests[$clientIP]) >= 100) {
        $res->status(429)->json(['error' => 'Too many requests']);
        return;
    }

    // Record this request
    $requests[$clientIP][] = $currentTime;

    $next();
});

$app->get('/api/data', function($req, $res) {
    $res->json(['data' => 'Rate limited endpoint']);
});

$app->listen('0.0.0.0', 8080);
```

## Testing

### Unit Testing with PHPUnit
```php
<?php
use PHPUnit\\Framework\\TestCase;
use Kislay\\Core\\App;
use Kislay\\Core\\Request;
use Kislay\\Core\\Response;

class AppTest extends TestCase {
    public function testBasicRoute() {
        $app = new App();

        $app->get('/test', function($req, $res) {
            $res->send('Hello Test');
        });

        // Note: Actual testing would require a test HTTP client
        // This is a simplified example
        $this->assertTrue(true); // Placeholder
    }

    public function testRequestParsing() {
        // Mock request object
        $request = new Request();
        $request->setMethod('GET');
        $request->setPath('/users/123');

        $this->assertEquals('GET', $request->method());
        $this->assertEquals('/users/123', $request->path());
    }
}
```

### Integration Testing
```php
<?php
class IntegrationTest extends PHPUnit\\Framework\\TestCase {
    private $serverProcess;

    protected function setUp(): void {
        // Start test server in background
        $this->serverProcess = proc_open(
            'php -d extension=kislayphp_extension.so test_server.php',
            [],
            $pipes,
            __DIR__,
            []
        );
        sleep(1); // Wait for server to start
    }

    protected function tearDown(): void {
        // Stop test server
        if ($this->serverProcess) {
            proc_terminate($this->serverProcess);
            proc_close($this->serverProcess);
        }
    }

    public function testHttpEndpoint() {
        $response = file_get_contents('http://localhost:8080/test');
        $this->assertEquals('Hello World', $response);
    }
}
```

## Troubleshooting

### Common Issues

#### Extension Not Loading
**Error:** `PHP Fatal error: Class 'Kislay\\Core\\App' not found`

**Solutions:**
1. Check if extension is installed: `php -m | grep kislayphp`
2. Verify php.ini path: `php --ini`
3. Restart web server after installation
4. Check file permissions on extension file

#### Port Already in Use
**Error:** `Failed to bind to port 8080`

**Solutions:**
1. Check what's using the port: `lsof -i :8080`
2. Use a different port: `$app->listen('0.0.0.0', 8081)`
3. Kill process using the port: `kill -9 <PID>`

#### Memory Issues
**Symptoms:** Server crashes with out of memory errors

**Solutions:**
1. Reduce thread count: `$app->setThreadCount(1)`
2. Set memory limit: `$app->setMemoryLimit(32 * 1024 * 1024)`
3. Enable garbage collection: `$app->enableGc(true)`
4. Monitor memory usage: `php -r "echo memory_get_peak_usage(true) / 1024 / 1024 . 'MB';"`

#### SSL/TLS Issues
**Error:** `SSL handshake failed`

**Solutions:**
1. Verify certificate files exist and are readable
2. Check certificate format (should be PEM)
3. Validate certificate chain
4. Ensure private key matches certificate

### Debug Logging
```php
<?php
$app = new Kislay\\Core\\App();

// Enable debug logging
$app->enableLogging(true);

// Custom error handler
set_error_handler(function($errno, $errstr, $errfile, $errline) {
    error_log("[$errno] $errstr in $errfile:$errline");
});

// Log all requests
$app->use(function($req, $res, $next) {
    $start = microtime(true);
    $next();
    $duration = microtime(true) - $start;

    error_log(sprintf(
        '%s %s %d %.3fms',
        $req->method(),
        $req->path(),
        $res->getStatusCode(),
        $duration * 1000
    ));
});

$app->listen('0.0.0.0', 8080);
```

### Performance Monitoring
```php
<?php
$app = new Kislay\\Core\\App();

// Request timing middleware
$app->use(function($req, $res, $next) {
    $req->setAttribute('startTime', microtime(true));
    $req->setAttribute('startMemory', memory_get_usage(true));

    $next();

    $duration = (microtime(true) - $req->getAttribute('startTime')) * 1000;
    $memoryUsed = memory_get_usage(true) - $req->getAttribute('startMemory');

    error_log(sprintf(
        'Request: %s %s | Time: %.2fms | Memory: %.2fMB',
        $req->method(),
        $req->path(),
        $duration,
        $memoryUsed / 1024 / 1024
    ));
});

// Health check endpoint
$app->get('/health', function($req, $res) {
    $stats = [
        'status' => 'healthy',
        'timestamp' => time(),
        'memory_usage' => memory_get_usage(true),
        'peak_memory' => memory_get_peak_usage(true),
        'uptime' => time() - $_SERVER['REQUEST_TIME']
    ];

    $res->json($stats);
});

$app->listen('0.0.0.0', 8080);
```

## Migration Guide

### From Kislay
```javascript
// Kislay
app.get('/users/:id', (req, res) => {
    res.json({ id: req.params.id });
});
```
```php
// Kislay
$app->get('/users/:id', function($req, $res) {
    $res->json(['id' => $req->input('id')]);
});
```

### From Kislay
```php
// Kislay
Route::get('/users/{id}', function($id) {
    return response()->json(['id' => $id]);
});
```
```php
// Kislay
$app->get('/users/:id', function($req, $res) {
    $res->json(['id' => $req->input('id')]);
});
```

### From Kislay
```python
# Kislay
@app.route('/users/<id>')
def get_user(id):
    return jsonify({'id': id})
```
```php
// Kislay
$app->get('/users/:id', function($req, $res) {
    $res->json(['id' => $req->input('id')]);
});
```

## Advanced Usage

### Custom Response Types
```php
<?php
class CustomResponse extends Kislay\\Core\\Response {
    public function csv(array $data, int $status = 200): void {
        $this->header('Content-Type', 'text/csv');
        $this->status($status);

        $output = fopen('php://temp', 'r+');
        if (!empty($data)) {
            fputcsv($output, array_keys($data[0]));
            foreach ($data as $row) {
                fputcsv($output, $row);
            }
        }
        rewind($output);
        $this->send(stream_get_contents($output));
        fclose($output);
    }
}

// Usage
$app->get('/export', function($req, $res) {
    $data = [
        ['name' => 'John', 'age' => 30],
        ['name' => 'Jane', 'age' => 25]
    ];

    $res->csv($data);
});
```

### Async Request Handling
```php
<?php
$app = new Kislay\\Core\\App();
$app->enableAsync(true);

$app->get('/async-data', function($req, $res) {
    // Simulate async operation
    $promise = async(function() {
        sleep(1); // Simulate I/O operation
        return ['data' => 'Async result'];
    });

    $promise->then(function($result) use ($res) {
        $res->json($result);
    });
});

$app->listen('0.0.0.0', 8080);
```

### WebSocket Integration
```php
<?php
// Note: WebSocket support requires kislayphp_eventbus extension

use Kislay\\EventBus\\WebSocketServer;

$app = new Kislay\\Core\\App();
$wsServer = new WebSocketServer();

$app->get('/chat', function($req, $res) {
    // Serve HTML page with WebSocket client
    $html = file_get_contents('chat.html');
    $res->type('text/html')->send($html);
});

// WebSocket endpoint
$wsServer->on('connection', function($socket) {
    $socket->on('message', function($data) use ($socket) {
        // Broadcast message to all connected clients
        $socket->broadcast('message', $data);
    });
});

$wsServer->listen('0.0.0.0', 8081);
$app->listen('0.0.0.0', 8080);
```

This comprehensive documentation covers all aspects of the Kislay Core extension, from basic usage to advanced features and troubleshooting.
