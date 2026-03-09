# Installation

## Requirements

Before installing KislayPHP extensions make sure your system has:

| Requirement | Minimum version |
|---|---|
| PHP | 8.2+ (with development headers) |
| C++ compiler | GCC 9+ / Clang 11+ / MSVC 2019+ (C++17 support) |
| OpenSSL | dev headers (`libssl-dev` on Debian/Ubuntu) |
| libcurl | dev headers (`libcurl4-openssl-dev`) |
| CMake | 3.16+ (for building from source) |

---

## Via Composer

Each extension ships as a PHP package that bundles its compiled `.so` file for Linux x86-64 and macOS arm64/x86-64.

```bash
# Core HTTP server (always required)
composer require kislayphp/core

# Optional extensions — install only what you need
composer require kislayphp/gateway
composer require kislayphp/discovery
composer require kislayphp/eventbus
composer require kislayphp/queue
composer require kislayphp/metrics
composer require kislayphp/persistence
composer require kislayphp/config
```

The Composer post-install script copies the correct `.so` into your PHP extension directory automatically.

---

## Building from Source

If your platform is not covered by pre-built binaries, build directly from the GitHub repository.

```bash
# Clone the extension you want, e.g. core
git clone https://github.com/KislayPHP/php-kislay-core
cd php-kislay-core

# Bootstrap the PHP extension build system
phpize

# Configure — enable optional features as needed
./configure --enable-kislay-core

# Compile
make -j$(nproc)

# Install into the active PHP installation
sudo make install
```

Then add to your `php.ini`:

```ini
extension=kislay_core.so
```

Repeat for each extension repository.

---

## Docker

A ready-to-use `docker-compose.production.yml` lives in the monorepo root. It bundles PHP 8.2 with all extensions pre-installed.

```bash
docker compose -f docker-compose.production.yml up
```

The image is built from `Dockerfile.prod` which runs the full `phpize` / `make` / `make install` cycle at build time, so you always get the latest compiled code.

---

## Verify Installation

```bash
php -m | grep -i kislay
```

Expected output (for a full install):

```
kislay_config
kislay_core
kislay_discovery
kislay_eventbus
kislay_gateway
kislay_metrics
kislay_persistence
kislay_queue
```

If an extension is missing, check `php --ini` to locate the active `php.ini` and ensure the `extension=` line was added.
