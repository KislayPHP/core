# KislayPHP Core

KislayPHP Core is a C++ PHP extension that provides a compact HTTP/HTTPS server with routing and middleware for building lightweight microservices.

## Key Features

- HTTP/HTTPS server built on civetweb.
- Routing with parameters and middleware support.
- Request and response helpers for JSON, headers, and status codes.
- Configurable via INI and environment values.

## Use Cases

- Local development servers for service prototypes.
- Small API services that need low overhead.
- Embedded admin endpoints for existing PHP apps.

## SEO Keywords

PHP HTTP server, HTTPS server, C++ PHP extension, routing middleware, lightweight API, embedded server, microservices

## Repository

- https://github.com/KislayPHP/core

## Related Modules

- https://github.com/KislayPHP/eventbus
- https://github.com/KislayPHP/discovery
- https://github.com/KislayPHP/gateway
- https://github.com/KislayPHP/config
- https://github.com/KislayPHP/metrics
- https://github.com/KislayPHP/queue

## Installation

### Dependencies

- civetweb (vendored in this repo)
- OpenSSL development headers

On macOS (Homebrew):

```sh
brew install openssl
```

1. Clone the repository:
   ```sh
   git clone https://github.com/KislayPHP/core.git
   cd core
   ```

2. Prepare the project for building:
   ```sh
   phpize
   ./configure --enable-kislayphp_extension
   make
   sudo make install
   ```

3. Run the example from this repo (no system install needed):
   ```sh
   cd /path/to/core
   php -d extension=modules/kislay_extension.so example.php
   ```

4. Add the extension to your `php.ini` file:
   ```ini
   extension=kislayphp_extension.so
   ```

5. Run the example script to test the extension:
   ```sh
   php example.php
   ```

## Usage

```php
<?php

extension_loaded('kislayphp_extension') or die('The kislayphp_extension is not loaded');

$app = new KislayPHP\Core\App();

$app->use(function ($req, $res, $next) {
   $next();
});

$app->get('/user/:id', function ($req, $res) {
   $params = $req->getParams();
   $res->status(200)->send('User ID: ' . $params['id']);
});

$app->post('/submit', function ($req, $res) {
   $res->json(['received' => $req->getBody()], 200);
});

$app->listen('0.0.0.0', 8080);

// HTTPS example
// $app->listen('0.0.0.0', 8443, ['cert' => '/path/to/cert.pem', 'key' => '/path/to/key.pem']);

?>
```

## Notes

- The HTTP/HTTPS server is experimental. PHP callbacks are invoked from civetweb worker threads, which can be unsafe depending on PHP build options.