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

## Via PIE (recommended)

Every extension ships as a real PHP source package (`"type": "php-ext"` in
its `composer.json`) built and installed with
[PIE](https://github.com/php/pie), PHP's own extension installer. **There
are no pre-built binaries today** — PIE compiles the extension from source
on your machine, so you still need a C++ compiler and the dependencies
below installed first.

```bash
# macOS (Homebrew)
brew install libuv

# Debian/Ubuntu
sudo apt-get update
sudo apt-get install -y pkg-config libcurl4-openssl-dev libssl-dev libuv1-dev
```

```bash
pie install kislayphp/core:1.0.1

# Optional extensions — install only what you need
pie install kislayphp/gateway
pie install kislayphp/discovery
pie install kislayphp/eventbus
pie install kislayphp/queue
pie install kislayphp/metrics
pie install kislayphp/persistence
pie install kislayphp/config
```

Then add the line PIE prints for you to `php.ini`, e.g.:

```ini
extension=kislayphp_extension.so
```

**Automation note:** in a non-interactive session on macOS, PIE may stop
after the build step because its final install step needs `sudo`. The
built module can still be validated directly from PIE's working directory
before that last interactive step.

---

## Building from Source

If you'd rather not use PIE, clone and build any extension directly:

```bash
git clone https://github.com/KislayPHP/core.git
cd core
phpize
./configure --enable-kislayphp_extension
make -j$(nproc)
sudo make install
```

Repeat for each extension repository (`gateway`, `socket`, `discovery`,
`queue`, `metrics`, `persistence`, `config`, `eventbus`), swapping the
`--enable-*` flag for that module's own configure option.

---

## Docker

There is no published production image yet, but the `quickstart/`
directory in the workspace root has a verified, working
`docker-compose.yml` + `Dockerfile` that builds Core from source inside
the container and runs a real Hello World server — no PHP or C++
toolchain needed on your host:

```bash
cd quickstart
docker compose up --build
curl http://localhost:8080/
```

This is a **quickstart/dev example, not a production-hardened image**
(no image-size optimization, no non-root user, no TLS) — see its own
README for details and caveats. `Dockerfile.zts-sigbus` in the workspace
root is unrelated: a debugging/dev environment (Linux/ZTS build for
chasing a specific platform-only bug), not something to run a real app
from.

---

## Verify Installation

```bash
php -m | grep -i kislay
```

Expected output (for a full install — module names, not package names):

```
kislayphp_config
kislayphp_discovery
kislayphp_eventbus
kislayphp_extension    # core
kislayphp_gateway
kislayphp_metrics
kislayphp_persistence
kislayphp_queue
kislayphp_socket
```

If an extension is missing, check `php --ini` to locate the active `php.ini` and ensure the `extension=` line was added.
