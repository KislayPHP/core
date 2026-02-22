# KislayPHP Deployment Guide (Docker, AWS, Cloud, On-Prem)

This runbook describes production deployment patterns for the KislayPHP extension stack:

- `core`
- `gateway`
- `discovery`
- `config`
- `metrics`
- `queue`
- `eventbus`

## 1. Reference Architecture

## 1.1 Control and Data Plane

- `gateway`: external API entrypoint
- `discovery`: service registry and service resolution
- `core` services: business APIs (docs/blog/auth/community/etc.)
- `config`: runtime configuration store/client
- `metrics`: counters and telemetry
- `queue`: async workload offload
- `eventbus`: real-time events and pub/sub

## 1.2 Recommended Ports

- `gateway`: `9008`
- `discovery`: `9090`
- Service ports: `91xx` (example: `9101-9109`)
- EventBus: choose dedicated real-time port range (example `9300+`)

## 2. Common Prerequisites

- PHP `8.2+`
- PIE installed (recommended) or source build toolchain (`phpize`, compiler, make)
- Linux host kernel/network tuning for high concurrency
- Reverse proxy/LB (Nginx, ALB, NLB, or cloud LB)
- TLS cert management (ACME or managed cert service)

Install extensions:

```bash
pie install kislayphp/core
pie install kislayphp/gateway
pie install kislayphp/discovery
pie install kislayphp/config
pie install kislayphp/metrics
pie install kislayphp/queue
pie install kislayphp/eventbus
```

`php.ini` baseline:

```ini
extension=kislayphp_extension.so
extension=kislayphp_gateway.so
extension=kislayphp_discovery.so
extension=kislayphp_config.so
extension=kislayphp_metrics.so
extension=kislayphp_queue.so
extension=kislayphp_eventbus.so
```

## 3. Docker Deployment

## 3.1 Single Host (Docker Compose)

Use one service per container:

- `registry` container
- `gateway` container
- app service containers (`docs`, `blog`, `auth`, `community`)

Checklist:

- run each service with explicit `SERVICE_URL`
- wire all services to the same Docker network
- expose only `gateway` publicly
- keep `discovery` internal-only

Minimal compose shape:

```yaml
services:
  registry:
    image: kislayphp/runtime:latest
    command: ["php", "registry_server.php"]
    ports: ["9090:9090"]

  docs:
    image: kislayphp/runtime:latest
    environment:
      REGISTRY_URL: http://registry:9090
      SERVICE_NAME: docs-service
      SERVICE_URL: http://docs:9101
    command: ["php", "docs_service.php"]

  gateway:
    image: kislayphp/gateway:latest
    environment:
      REGISTRY_URL: http://registry:9090
      GATEWAY_PORT: 9008
    ports: ["9008:9008"]
```

## 3.2 Container Hardening

- run as non-root user
- read-only root filesystem where possible
- pin image tags
- enable health checks per container
- ship logs to centralized sink

## 4. AWS Deployment

## 4.1 EC2 (Fastest Path)

Recommended for first production rollout.

Steps:

1. Provision Ubuntu EC2 with security groups:
   - public: `80/443` (or `9008` for direct testing)
   - internal-only: `9090`, `91xx`
2. Install PHP + PIE and extensions.
3. Deploy each service as systemd unit.
4. Place Nginx in front of gateway.
5. Enable CloudWatch agent for logs and metrics.

Systemd unit pattern:

```ini
[Service]
ExecStart=/usr/bin/php /opt/kislay/services/docs_service.php
Restart=always
Environment=REGISTRY_URL=http://127.0.0.1:9090
Environment=SERVICE_NAME=docs-service
Environment=SERVICE_URL=http://127.0.0.1:9101
```

## 4.2 ECS Fargate

Use when you want managed containers without host ops.

- task per service (`registry`, `gateway`, each API service)
- internal service discovery or fixed internal DNS
- ALB -> gateway task
- restrict `registry` to private subnets

## 4.3 EKS (Kubernetes)

Use when you need multi-team platform scale.

- `Deployment` + `Service` for each component
- gateway behind `Ingress`/`LoadBalancer`
- discovery as internal ClusterIP service
- autoscale gateway and stateless app pods
- use ConfigMaps/Secrets for runtime config

## 5. Cloud-Agnostic VM Deployment (GCP/Azure/DO/etc.)

Pattern is same as EC2:

- private network for internal services
- only gateway/LB public
- reverse proxy + TLS termination
- per-service process manager (`systemd`, `supervisord`)
- centralized logs + metrics backend

## 6. On-Premises Deployment

## 6.1 Bare Metal / VM

- place gateway in DMZ tier
- keep registry/config/queue internal VLAN
- enforce firewall ACLs between tiers
- run active/passive or active/active gateway nodes

## 6.2 On-Prem Kubernetes/OpenShift

- mirror container images to internal registry
- deploy with Helm/Kustomize
- persistent backing stores for config/queue if externalized
- internal ingress for east-west service traffic

## 7. Production Baseline

## 7.1 Reliability

- health endpoint on every service: `/health`
- readiness checks at process and route levels
- restart policy (`always`) and backoff
- rolling deploy with at least 2 gateway replicas

## 7.2 Security

- TLS at edge, optional mTLS internally
- strict ingress policy to gateway only
- sanitize auth tokens and secrets in logs
- enable `Referrer-Policy` and CORS only as needed

## 7.3 Observability

- request log fields:
  - timestamp
  - method/path
  - status
  - duration
  - error (if any)
- gateway metrics: RPS, p95 latency, upstream failures
- discovery metrics: registrations, heartbeat failures, resolve misses

## 7.4 Performance Tuning

- NTS PHP: keep thread settings conservative
- use async/background workers for heavy jobs (`queue`)
- use gateway-level rate limits and upstream timeout controls
- benchmark before each release

## 8. Release and Rollback Strategy

## 8.1 Release

1. Build and test extensions in CI.
2. Publish tagged releases (`0.0.x` while stabilizing).
3. Roll out canary on one node.
4. Promote after smoke and load checks.

## 8.2 Rollback

- keep previous package/version on host
- switch service symlink or container tag
- restart only affected service
- verify with gateway smoke endpoints

## 9. Smoke Test Checklist (Any Environment)

- `GET /health` on registry and each service
- `GET /v1/services` on discovery
- `GET /api/site/home` via gateway
- login flow via gateway (`/api/auth/login` + `/api/auth/me`)
- monitor logs for 4xx/5xx spikes

## 10. Recommended Next Step

Start with EC2 single-node or Docker Compose, stabilize traffic and observability, then move to ECS/EKS or on-prem K8s when service count and team size require it.
