# KislayPHP Core

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Performance](https://img.shields.io/badge/Performance-63K%20req%2Fsec-orange.svg)]()
[![Build Status](https://img.shields.io/github/actions/workflow/status/KislayPHP/core/ci.yml)](https://github.com/KislayPHP/core/actions)

A high-performance C++ PHP extension providing a compact HTTP/HTTPS server with routing and middleware for building lightweight microservices and APIs. Built on CivetWeb with full PHP compatibility and enterprise-grade performance.

## ⚡ Key Features

- 🚀 **High Performance**: 63K requests/second at 200 concurrency with 0.25ms P95 latency
- 🔒 **Secure**: Built-in HTTPS/SSL support with TLS encryption
- 🛠️ **Full PHP Compatibility**: Complete PHP processing without fast-path bypass
- 🏗️ **Microservices Ready**: Lightweight server for containerized deployments
- 🔧 **Configurable**: Environment-based configuration and INI settings
- 📊 **Production Ready**: Comprehensive logging and monitoring support
- 🔄 **PHP Ecosystem**: Seamless integration with PHP frameworks
- 🌐 **Distributed**: Designed for microservices architecture

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
./configure --enable-kislayphp_extension
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
$app = new KislayPHP\Core\App();

// Configure server
$app->setOption('num_threads', 4);
$app->setOption('document_root', '/var/www');

// Add middleware
$app->use(function ($req, $res, $next) {
    $next();
});

// Define routes
$app->get('/api/users', function ($req, $res) {
    $users = [
        ['id' => 1, 'name' => 'John Doe'],
        ['id' => 2, 'name' => 'Jane Smith']
    ];
    $res->json($users);
});

$app->post('/api/users', function ($req, $res) {
    $data = $req->getJson();
    $res->json(['status' => 'created', 'id' => 123]);
});

// Start server
$app->listen('0.0.0.0', 8080);
```

## 🏗️ Architecture

KislayPHP Core implements a hybrid threading model combining PHP execution with asynchronous I/O:

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

## 📊 Performance

**Benchmark Results (200 concurrent connections):**
- **Throughput**: 63K requests/second
- **P95 Latency**: 0.25ms
- **P99 Latency**: 0.45ms
- **CPU Usage**: 11.4%
- **Memory**: 6.0MB (stable)

## 🔧 Configuration

### php.ini Settings

```ini
kislayphp.num_threads = 4
kislayphp.document_root = "/var/www"
kislayphp.enable_ssl = 0
kislayphp.ssl_cert = "/path/to/cert.pem"
kislayphp.ssl_key = "/path/to/key.pem"
kislayphp.max_request_size = 1048576
kislayphp.request_timeout = 30
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
# Run tests
php run-tests.php

# Run benchmarks
cd tests/
php benchmark.php --duration=60 --concurrency=100
```

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](.github/CONTRIBUTING.md).

## 📄 License

Licensed under the [Apache License 2.0](LICENSE).

## 🆘 Support

- 📖 [Documentation](docs.md)
- 🐛 [Issues](https://github.com/KislayPHP/core/issues)
- 💬 [Discussions](https://github.com/KislayPHP/core/discussions)

## 🙏 Acknowledgments

- **CivetWeb**: Embedded HTTP server library
- **PHP**: Zend API for extension development
- **OpenSSL**: SSL/TLS encryption support

---

**Built with ❤️ for the PHP community**