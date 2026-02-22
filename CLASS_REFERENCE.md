# Kislay Core Class Reference

This document describes runtime classes exported by `kislayphp/core`.

## Namespace

- Primary namespace: `Kislay\\Core`
- Classes:
  - `App`
  - `Request`
  - `Response`
  - `Next`
  - `AsyncHttp`

## `Kislay\\Core\\App`

Primary HTTP server object for routes, middleware, lifecycle, and runtime options.

### Constructor

- `__construct()`
  - Creates a new app instance with runtime defaults from INI/env.

### Routing

- `get(string $path, callable $handler)`
  - Register a GET route.
- `post(string $path, callable $handler)`
  - Register a POST route.
- `put(string $path, callable $handler)`
  - Register a PUT route.
- `patch(string $path, callable $handler)`
  - Register a PATCH route.
- `delete(string $path, callable $handler)`
  - Register a DELETE route.
- `options(string $path, callable $handler)`
  - Register an OPTIONS route.
- `all(string $path, callable $handler)`
  - Register a handler for all methods.

### Middleware and Grouping

- `use($pathOrMiddleware, ?callable $middleware = null)`
  - Register global or path-scoped middleware.
- `group(string $prefix, callable $callback, ?array $middleware = null)`
  - Register grouped routes under a path prefix with optional middleware.

### Server Lifecycle

- `listen(string $host, int $port, ?array $tls = null)`
  - Start server in blocking mode.
- `listenAsync(string $host, int $port, ?array $tls = null)`
  - Start server in non-blocking mode.
- `wait(?int $timeoutMs = null)`
  - Wait for server events when running async.
- `stop()`
  - Stop the server.
- `isRunning()`
  - Check whether the server is active.

### Runtime Controls

- `setOption(string $key, mixed $value)`
  - Configure runtime options (threads, timeouts, CORS, referrer policy, logging, limits, TLS defaults, document root).
- `enableAsync(bool $enabled)`
  - Enable/disable async mode behavior.
- `enableGc(bool $enabled)`
  - Enable/disable garbage collection integration.
- `setMemoryLimit(int $bytes)`
  - Set memory limit in bytes.
- `getMemoryLimit()`
  - Return current memory limit.

## `Kislay\\Core\\Request`

Request wrapper for path, method, body, query, headers, and attributes.

### Body and Input

- `all()`
  - Return merged request payload.
- `body()`
  - Return request body helper output.
- `getBody()`
  - Return raw body string.
- `getJson(mixed $default = null)`
  - Decode JSON body, or return default.
- `json(mixed $default = null)`
  - Alias for JSON access.
- `isJson()`
  - Check if request content is JSON.
- `input(string $name, mixed $default = null)`
  - Get a single input field.
- `only(array $keys)`
  - Return subset of payload keys.
- `has(string $name)`
  - Check whether payload key exists.

### Path, Method, Query

- `getMethod()` / `method()`
  - Return HTTP method.
- `getPath()` / `path()`
  - Return request path.
- `getUri()`
  - Return full URI/path value.
- `getQuery()`
  - Return full query map.
- `getQueryParams()`
  - Return query params map.
- `query(string $name, mixed $default = null)`
  - Get a query parameter.
- `getParams()`
  - Return route/path params.

### Headers

- `getHeader(string $name)`
  - Return a header value.
- `getHeaders()`
  - Return all headers.
- `header(string $name, mixed $default = null)`
  - Header helper accessor.
- `hasHeader(string $name)`
  - Check if header exists.

### Attributes

- `setAttribute(string $name, mixed $value)`
  - Set middleware/route attribute.
- `getAttribute(string $name, mixed $default = null)`
  - Read attribute.
- `hasAttribute(string $name)`
  - Check if attribute exists.

## `Kislay\\Core\\Response`

Response builder for status, headers, typed payloads, and convenience HTTP helpers.

### Body and Status

- `send(mixed $body, ?int $status = null)`
  - Send text/body payload.
- `json(mixed $data, ?int $status = null)`
  - Send JSON response.
- `sendJson(mixed $data, ?int $status = null)`
  - JSON alias.
- `xml(string $xml, ?int $status = null)`
  - Send XML response.
- `sendXml(string $xml, ?int $status = null)`
  - XML alias.
- `sendFile(string $filePath, ?string $contentType = null, ?int $status = null)`
  - Send static file.
- `status(int $status)`
  - Set status code fluent-style.
- `sendStatus(int $status)`
  - Send only status code.
- `setStatusCode(int $status)`
  - Set status code.
- `getStatusCode()`
  - Read status code.
- `setBody(string $body)`
  - Set response body.
- `getBody()`
  - Read response body.

### Headers and Content Type

- `setHeader(string $name, string $value)`
  - Set a header.
- `header(string $name, string $value)`
  - Header alias.
- `set(string $name, string $value)`
  - Header alias.
- `type(string $contentType)`
  - Set content type.

### Convenience Status Helpers

- `ok(mixed $default = null)`
- `created(mixed $default = null)`
- `noContent()`
- `badRequest(mixed $default = null)`
- `unauthorized(mixed $default = null)`
- `forbidden(mixed $default = null)`
- `notFound(mixed $default = null)`
- `methodNotAllowed(mixed $default = null)`
- `conflict(mixed $default = null)`
- `unprocessableEntity(mixed $default = null)`
- `internalServerError(mixed $default = null)`

Each helper sets the matching HTTP status and optional body.

## `Kislay\\Core\\Next`

Middleware continuation callback wrapper.

- `__invoke()`
  - Continue middleware chain.

## `Kislay\\Core\\AsyncHttp`

Async HTTP client helper for non-blocking upstream calls.

### Constructor

- `__construct()`
  - Create client object.

### Request Methods

- `get(string $url, ?array $data = null)`
- `post(string $url, ?array $data = null)`
- `put(string $url, ?array $data = null)`
- `patch(string $url, ?array $data = null)`
- `delete(string $url, ?array $data = null)`

Queue a request with optional payload.

### Headers and Execution

- `setHeader(string $name, string $value)`
  - Add request header.
- `execute()`
  - Execute queued async request(s).
- `getResponse()`
  - Return response body.
- `getResponseCode()`
  - Return HTTP status code.

## Notes

- Thread behavior depends on PHP build type (ZTS vs NTS).
- On NTS builds, thread-heavy settings are safely reduced with warnings.
- CORS and `Referrer-Policy` can be controlled with `setOption(...)`.
