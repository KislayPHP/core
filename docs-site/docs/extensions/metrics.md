# Metrics & Telemetry

## Overview

`kislayphp/metrics` adds a Prometheus-compatible metrics system to any KislayPHP application. Instrument your code with counters, gauges, and histograms; the extension exposes a `/metrics` scrape endpoint automatically. All metric updates are lock-free using C++ atomics, so there is no measurable overhead in the hot request path.

## Installation

```bash
composer require kislayphp/metrics
```

GitHub: <https://github.com/KislayPHP/php-kislay-metrics>

---

## Key Features

- **Counter** — monotonically increasing values (requests, errors)
- **Gauge** — arbitrarily up/down values (active connections, queue depth)
- **Histogram** — latency distributions with configurable buckets
- **Labels** — multi-dimensional metrics (method, status_code, route)
- **Prometheus `/metrics` endpoint** — auto-registered on the core HTTP server
- **Auto-instrumentation** — optional middleware records HTTP request duration / status
- **OpenTelemetry export** — OTLP gRPC export (optional, compile-time flag)

---

## Quick Example

```php
<?php
$app     = new Kislay\App();
$metrics = new Kislay\Metrics($app);

// Define metrics
$requestCounter  = $metrics->counter('http_requests_total', 'Total HTTP requests', ['method', 'status']);
$activeGauge     = $metrics->gauge('active_connections', 'Current active connections');
$latencyHist     = $metrics->histogram('request_duration_seconds', 'Request latency', ['route'],
                       [0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5]);

// Middleware for automatic instrumentation
$app->use(function ($req, $res, $next) use ($requestCounter, $activeGauge, $latencyHist) {
    $start = microtime(true);
    $activeGauge->inc();

    $next();

    $duration = microtime(true) - $start;
    $activeGauge->dec();
    $requestCounter->inc(['method' => $req->method, 'status' => $res->statusCode]);
    $latencyHist->observe($duration, ['route' => $req->path]);
});

$app->get('/', fn ($req, $res) => $res->json(['ok' => true]));
// GET /metrics  →  Prometheus text format (auto-registered)

$app->listen();
```

---

## Configuration

| Option | Env Var | Default | Description |
|---|---|---|---|
| `metrics_path` | `KISLAY_METRICS_PATH` | `/metrics` | Scrape endpoint path |
| `metrics_auth` | `KISLAY_METRICS_TOKEN` | — | Bearer token to protect `/metrics` |
| `auto_http` | — | `false` | Auto-instrument all HTTP routes |
| `otlp_endpoint` | `OTEL_EXPORTER_OTLP_ENDPOINT` | — | OTLP gRPC target URL |
| `default_buckets` | — | see above | Histogram bucket boundaries |
