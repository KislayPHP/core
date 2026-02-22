# Microservice Stack Quickstart

This starter runs a minimal service mesh on one machine:

- `ServiceRegistry` (Discovery)
- `user-service` and `order-service` (Core + Discovery)
- `Gateway` with discovery-based service resolution

## 1) Configure

```bash
cd examples/microservice-stack
cp .env.example .env
```

Edit `.env` if you want custom ports.

## 2) Start

```bash
./start.sh
```

## 3) Verify

```bash
./status.sh
```

Expected gateway routes:

- `GET /api/users`
- `GET /api/orders`

## 4) Stop

```bash
./stop.sh
```

## Notes

- By default, scripts auto-detect:
  - Core module in this repo: `modules/kislayphp_extension.so`
  - Discovery module in sibling repo: `../kislayphp_discovery/modules/kislayphp_discovery.so`
  - Gateway module in sibling repo: `../kislayphp_gateway/modules/kislayphp_gateway.so`
- If your workspace layout differs, set `WORKSPACE_ROOT` and/or `DISCOVERY_EXAMPLES` in `.env`.
- If you use system/PIE installs instead, leave those files absent or set explicit `CORE_SO`, `DISCOVERY_SO`, `GATEWAY_SO` values in `.env`.
