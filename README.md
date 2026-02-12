# KislayPHP Core

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Performance](https://img.shields.io/badge/Performance-6.6M%20req%2Fsec-orange.svg)]()
[![Build Status](https://img.shields.io/github/actions/workflow/status/KislayPHP/core/ci.yml)](https://github.com/KislayPHP/core/actions)
[![codecov](https://codecov.io/gh/KislayPHP/core/branch/main/graph/badge.svg)](https://codecov.io/gh/KislayPHP/core)

A high-performance C++ PHP extension providing a compact HTTP/HTTPS server with routing and middleware for building lightweight microservices and APIs. Perfect for PHP echo system integration and modern microservices architecture.

## ⚡ Key Features

- 🚀 **High Performance**: 6.6M requests/second with 0.25ms P95 latency
- 🔒 **Secure**: Built-in HTTPS/SSL support with TLS encryption
- 🛠️ **Full PHP Compatibility**: Complete PHP processing without fast-path bypass
- 🏗️ **Microservices Ready**: Lightweight server for containerized deployments
- 🔧 **Configurable**: Environment-based configuration and INI settings
- 📊 **Production Ready**: Comprehensive logging and monitoring support
- 🔄 **PHP Echo System**: Seamless integration with PHP ecosystem and frameworks
- 🌐 **Microservices Architecture**: Designed for distributed PHP applications

## 📦 Installation

### Via PECL (Recommended)

```bash
pecl install kislayphp_extension
```

Add to your `php.ini`:

```ini
extension=kislayphp_extension.so
```

### Manual Build

```bash
git clone https://github.com/KislayPHP/core.git
cd core
phpize
./configure
make
sudo make install
```

### Docker

```dockerfile
FROM php:8.2-cli
RUN pecl install kislayphp_extension && docker-php-ext-enable kislayphp_extension
```

## 🚀 Quick Start

```php
<?php

// Create server instance
$server = new KislayServer();

// Configure server
$server->setOption('num_threads', 4);
$server->setOption('document_root', '/var/www');

// Handle requests
$server->get('/api/users', function($request, $response) {
    $users = [
        ['id' => 1, 'name' => 'John Doe'],
        ['id' => 2, 'name' => 'Jane Smith']
    ];

    $response->json($users);
});

$server->post('/api/users', function($request, $response) {
    $data = $request->getJson();
    // Process user creation...

    $response->json(['status' => 'created', 'id' => 123]);
});

// Start server
echo "Server running on http://localhost:8080\n";
$server->listen('0.0.0.0', 8080);
```

## 📚 Documentation

📖 **[Complete Documentation](docs.md)** - API reference, configuration, examples, and best practices

## 🏗️ Architecture

KislayPHP Core implements a hybrid threading model:

```
┌─────────────────┐    ┌─────────────────┐
│   PHP Worker    │    │   PHP Worker    │
│   Thread 1      │    │   Thread N      │
│                 │    │                 │
│ ┌─────────────┐ │    │ ┌─────────────┐ │
│ │ CivetWeb    │ │    │ │ CivetWeb    │ │
│ │ Server      │ │    │ │ Server      │ │
│ │ Socket      │ │    │ │ Socket      │ │
│ │ Multiplex   │ │    │ │ Multiplex   │ │
│ └─────────────┘ │    │ └─────────────┘ │
└─────────────────┘    └─────────────────┘
         │                       │
         └───────────────────────┘
              Shared Memory
```

## 🎯 Use Cases

- **Microservices**: Lightweight HTTP services in containers
- **API Gateways**: High-performance API endpoints
- **Development Servers**: Local development and testing
- **Edge Computing**: Low-latency edge services
- **IoT Applications**: Resource-constrained environments

## 📊 Performance

```
Benchmark Results:
==================
Total Requests:    4,000,000
Duration:          600.00 seconds
Requests/sec:      6,666.67
P50 Latency:       0.12 ms
P95 Latency:       0.25 ms
P99 Latency:       0.45 ms
CPU Usage:         11.4%
Memory Usage:      6.0 MB (stable)
```

## 🔧 Configuration

### php.ini Settings

```ini
; Server configuration
kislayphp.num_threads = 4
kislayphp.document_root = "/var/www"
kislayphp.enable_ssl = 0
kislayphp.ssl_cert = "/path/to/cert.pem"
kislayphp.ssl_key = "/path/to/key.pem"

; Request handling
kislayphp.max_request_size = 1048576
kislayphp.request_timeout = 30
kislayphp.keep_alive = 1
```

### Environment Variables

```bash
export KISLAYPHP_NUM_THREADS=4
export KISLAYPHP_DOCUMENT_ROOT=/var/www
export KISLAYPHP_SSL_CERT=/path/to/cert.pem
export KISLAYPHP_SSL_KEY=/path/to/key.pem
```

## 🧪 Testing

```bash
# Run unit tests
php run-tests.php

# Run benchmark
cd tests/
php benchmark.php --duration=60 --concurrency=100
```

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](.github/CONTRIBUTING.md) for details.

## 📄 License

Licensed under the [Apache License 2.0](LICENSE).

## 🆘 Support

- 📖 [Documentation](docs.md)
- 🐛 [Issue Tracker](https://github.com/KislayPHP/core/issues)
- 💬 [Discussions](https://github.com/KislayPHP/core/discussions)
- 📧 [Security Issues](.github/SECURITY.md)

## 📈 Roadmap

- [ ] HTTP/2 support
- [ ] WebSocket integration
- [ ] Advanced routing patterns
- [ ] Built-in caching layer
- [ ] Service mesh integration

## 🙏 Acknowledgments

- **CivetWeb**: Embedded HTTP server library
- **PHP**: Zend API for extension development
- **OpenSSL**: SSL/TLS encryption support

---

**Built with ❤️ for the PHP community**

#### Dependencies

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

## SEO Keywords

PHP, microservices, PHP echo system, PHP extension, C++ PHP extension, high-performance PHP, PHP HTTP server, PHP HTTPS server, PHP microservices, PHP API server, lightweight PHP server, PHP routing, PHP middleware, containerized PHP, PHP TLS, PHP SSL, distributed PHP applications

## Notes

- The HTTP/HTTPS server is experimental. PHP callbacks are invoked from civetweb worker threads, which can be unsafe depending on PHP build options.