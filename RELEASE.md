# Release Guide

## Current release

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
