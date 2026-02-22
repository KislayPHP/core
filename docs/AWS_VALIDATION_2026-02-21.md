# AWS Validation Report (2026-02-21)

Host: `ec2-54-242-252-137.compute-1.amazonaws.com` (Ubuntu, PHP 8.3.6 NTS)

## Summary

- Full extension stack builds and loads successfully from latest GitHub `main`.
- Service communication works (`registry -> gateway -> services`) with discovery-based routing.
- Current Packagist/PIE artifacts are behind source for some modules, causing install-time failures.

## Build + Load Results (GitHub main)

All passed:

- `core` (`kislayphp_extension`)
- `gateway` (`kislayphp_gateway`)
- `discovery` (`kislayphp_discovery`)
- `queue` (`kislayphp_queue`)
- `metrics` (`kislayphp_metrics`)
- `config` (`kislayphp_config`)
- `eventbus` (`kislayphp_eventbus`)

## API Smoke Results

- Queue enqueue/dequeue: pass
- Metrics inc/dec/get: pass
- Config set/get/has: pass
- EventBus server object init: pass

## Communication Smoke

Validated on isolated ports:

- Registry health: pass
- Service registration visible on `/v1/services`: pass
- Gateway route `/api/users`: pass
- Gateway route `/api/orders`: pass

## Current Packagist/PIE Gaps

Observed from PIE install path:

- `kislayphp/eventbus` package build failed (missing C++ std headers in released source snapshot).
- `kislayphp/discovery` package configure failed due hard RPC stub requirement in released snapshot.

These failures are not present in latest GitHub source.

## Action Required Before Public Production Release

1. Publish fresh tags for discovery/eventbus/other modules from latest fixed sources.
2. Re-sync Packagist metadata and verify `pie install` end-to-end on clean Linux host.
3. Keep `scripts/aws_validate_extensions.sh` as pre-release gate.
