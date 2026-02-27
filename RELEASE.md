# Release Guide

## Recent Releases

### v0.0.2 (2026-02-28)
- **Multi-threaded Worker Pool**: Added support for true parallel execution on ZTS PHP via `async_threads`.
- **Sensible Defaults**: Enabled `async`, `log`, and `enable_gc` by default.
- **Lifecycle Improvements**: Fixed race conditions in `stop()` and improved stability of `listenAsync()`.
- **Header Policies**: Added default `Referrer-Policy` and improved CORS preflight handling.
- **Version Alignment**: Synced extension version with PECL and Composer manifests.

### v0.0.1 (2026-02-21)
- Initial release with basic HTTP/HTTPS server and Promise-based async support.

## Pre-publish checks

Run from repository root:

```bash
chmod +x scripts/release_check.sh
./scripts/release_check.sh
```

## Build extension artifact

```bash
phpize
./configure
make -j4
```

## Publish checklist

- Confirm `README.md`, `composer.json`, and `package.xml` are up to date.
- Confirm `package.xml` release and API versions are set correctly.
- Confirm examples pass `php -n -l`.
- Tag release and push tag to origin.
