# Release Guide

## Current release

### v0.0.7 (2026-03-24)
- **Zero-copy response via `raw_ptr`**: `RuntimeResponseMessage` now carries `raw_ptr/raw_len/send_raw_buffer`; `mg_write()` reads directly from the buffer — one fewer `std::string` copy per response
- **`zend_string*` body field**: `_php_kislay_response_t` holds `body_zstr` so `send()`, `sendJson()`, `json()` store the PHP string by refcount instead of copying into a `std::string`
- **Connection: keep-alive**: all four response header sites changed from `Connection: close`, reducing per-request TCP overhead
- **Persistence extension MSHUTDOWN fix**: rebuilt `kislayphp_persistence.so` from current source; old binary had a stale `MSHUTDOWN` that called `zval_ptr_dtor` in a loop after `RSHUTDOWN` had already freed the same objects → double-free → `zend_mm_panic` → SIGABRT; `json/100k` throughput went from **648 → 13,134 req/s** (20×)
- **Removed broken thread-local buffer swap**: the 256 KB `tl_body_hint` swap in `drain()` and `worker_main()` caused an extra malloc/free per request with no benefit (buffer was moved to CivetWeb thread and could not be recovered); removed entirely

### v0.0.6 (2026-03-21)
- strict `:param` segment router in the hot path
- compiled middleware chains with explicit boolean continuation
- lower-allocation lazy query/body parsing
- hardened request reset and zval cleanup
- explicit AsyncHttp self-request guard in single-runtime mode
- PHPT suite aligned with the current NTS and ZTS runtime contract

### v0.0.5 (2026-02-28)
- request context safety improvements
- race-condition fixes around lifecycle operations

## Pre-publish checks

Run from repository root:

```bash
phpize
./configure --enable-kislayphp_extension
make -j4
php run-tests.php -q -n -d extension=modules/kislayphp_extension.so tests
```

## Release checklist

- update `php_kislay_extension.h`
- update `package.xml`
- update `README.md` and `docs.md`
- run local benchmarks on `/plaintext`, `/users/:id`, and `/submit/:id`
- tag and push release
