# Windows Support

KislayPHP extensions can be compiled and run on Windows using the official PHP SDK Binary Tools and Visual Studio Build Tools. This page covers the build prerequisites, step-by-step compilation, CI matrix configuration, and known limitations.

---

## Prerequisites

| Requirement | Version |
|---|---|
| Windows | 10 / 11 / Server 2019+ |
| Visual Studio Build Tools | 2019 or 2022 (Desktop C++ workload) |
| PHP SDK Binary Tools | <https://github.com/php/php-sdk-binary-tools> |
| PHP source | Matching the target PHP version |
| NMake | Bundled with Visual Studio |

---

## Build Steps

### 1. Open the PHP SDK shell

```cmd
git clone https://github.com/php/php-sdk-binary-tools C:\php-sdk
C:\php-sdk\phpsdk-vs17-x64.bat
```

This sets up the correct `PATH`, compiler environment, and `include` / `lib` directories.

### 2. Download PHP sources and dependencies

```cmd
phpsdk_buildtree php-8.2
cd php-8.2
phpsdk_deps --update --branch 8.2
```

### 3. Configure and build the extension

Inside the SDK shell, from the extension directory:

```cmd
phpize
configure --enable-kislay-core --with-prefix=C:\php
nmake
nmake install
```

### 4. Enable the extension

Add to `php.ini`:

```ini
extension=php_kislay_core.dll
```

---

## CI Matrix

Add Windows builds to your GitHub Actions workflow:

```yaml
strategy:
  matrix:
    os: [ubuntu-latest, windows-latest, macos-latest]
    php: ['8.2', '8.3']
```

Use `shivammathur/setup-php@v2` for cross-platform PHP setup:

```yaml
- uses: shivammathur/setup-php@v2
  with:
    php-version: ${{ matrix.php }}
    extensions: ''
```

---

## Known Limitations

- **POSIX signals** — `SIGTERM` / `SIGHUP` handling is not supported; use Windows Service Manager instead.
- **Fork-based workers** — worker threads are used instead of `fork()` on Windows.
- **Unix sockets** — not available; use TCP sockets only.
- **`proc_open` watcher** — the Config extension file watcher uses `ReadDirectoryChangesW` on Windows; Consul / etcd backends work normally.
- **OpenSSL path** — specify `--with-openssl=C:\OpenSSL-Win64` during `configure`.
