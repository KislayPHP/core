# Production Readiness Checklist

This checklist is for releasing the KislayPHP extension stack to production.

## Must Pass Before Release

- [ ] Build succeeds for all modules on Linux x86_64 using `phpize && ./configure && make`
- [ ] Module load smoke succeeds for all `.so` files using `php -n -d extension=...`
- [ ] API smoke tests pass for core, gateway, discovery, config, metrics, queue, and eventbus
- [ ] Service-to-service communication passes (`registry -> service registration -> gateway route -> backend service`)
- [ ] PHPT tests exist for every module and run in CI
- [ ] Packagist/PIE package versions point to the same Git references as release tags
- [ ] README examples match real exported APIs for each extension

## Runtime Defaults

- Thread Safety disabled: force single-thread fallback with warning (already supported)
- Invalid numeric options: fallback to safe defaults with warning (already supported in core)
- Request logging enabled for production troubleshooting:
  - `KISLAYPHP_HTTP_LOG=1`

## Operational Baseline

- Health endpoints exposed on every service (`/health`)
- Gateway endpoint smoke:
  - `/api/users`
  - `/api/orders`
- Discovery endpoints smoke:
  - `/v1/services`
  - `/v1/resolve?service=...`
- Structured logs captured for:
  - request method/path
  - response status/bytes
  - duration
  - error message (if any)

## AWS Validation

Use the automation script from workspace root:

```bash
./scripts/aws_validate_extensions.sh <key.pem> <host> <user>
```

The script validates:

1. GitHub source build for all extension repos
2. Extension load for each module
3. Basic API smoke for queue/metrics/config/eventbus
4. Discovery + gateway communication flow
