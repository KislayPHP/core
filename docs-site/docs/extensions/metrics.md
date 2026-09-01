# Metrics & Telemetry

## Overview

`kislayphp/metrics` adds a Prometheus-compatible metrics system to any KislayPHP application. Instrument your code with counters, gauges, and histograms; the extension exposes a `/metrics` scrape endpoint automatically. All metric updates are lock-free using C++ atomics, so there is no measurable overhead in the hot request path.

## Installation

There are no pre-built binaries yet — this compiles from source via [PIE](https://github.com/php/pie) or a manual `phpize` build, the same as core. See [Installation](../getting-started/installation.md).

GitHub: <https://github.com/KislayPHP/metrics>

---

## Key Features

- **Counter** — monotonically increasing values (requests, errors)
- **Gauge** — arbitrarily up/down values (active connections, queue depth)
- **Histogram** — latency distributions with configurable buckets
- **Prometheus `/metrics` endpoint** — via the bundled `PrometheusExporter` PHP helper (`registerEndpoint()`), not automatic
- **Lock-free hot path** — all updates are C++ atomics

There is currently **no labels support** (a counter/gauge is a single flat named value — `increment($by = 1)` takes no label array) and **no OpenTelemetry/OTLP export** — both would need to be verified against a specific release before relying on them; neither exists in the current C++ source.

---

## Quick Example

```php
<?php
use Kislay\Metrics\Metrics;
use Kislay\Metrics\PrometheusExporter;

$app     = new Kislay\Core\App();
$metrics = new Metrics();

// Global middleware
$app->use(function ($req, $res, $next) use ($metrics) {
    $metrics->inc('http_requests_total');
    $next();
});

$app->get('/', fn ($req, $res) => $res->json(['ok' => true]));

// Registers GET /metrics returning Prometheus text format
PrometheusExporter::registerEndpoint($app, $metrics, '/metrics', 'myapp');

$app->listen('0.0.0.0', 8080);
```

For typed Counter/Gauge/Histogram/Timer objects instead of `Metrics`'s flat string-keyed API, use `Kislay\Metrics\Collector`:

```php
$collector = new Kislay\Metrics\Collector('myapp');
$requests  = $collector->counter('http_requests_total');   // name only, no help text/labels
$latency   = $collector->histogram('request_duration_seconds', [0.005, 0.01, 0.05, 0.1, 0.5, 1, 2.5]);

$requests->increment();
$latency->observe(0.042);

echo $collector->export(); // Prometheus text format for everything registered on this Collector
```

---

## Verified API

| Class | Methods |
|---|---|
| `Kislay\Metrics\Metrics` | `inc(string $name, int $by = 1)`, `dec(...)`, `get(string $name)`, `all()`, `reset(?string $name = null)` |
| `Kislay\Metrics\Collector` | `__construct(string $namespace = '')`, `counter(string $name)`, `gauge(string $name)`, `histogram(string $name, array $buckets = [])`, `timer(string $name)`, `export()` |
| `Kislay\Metrics\Counter` / `Gauge` | `increment(int $by = 1)`, `decrement(int $by = 1)`, `get()`, `reset()` |
| `Kislay\Metrics\Histogram` | `observe(float $value)`, `getCount()`, `getSum()`, `getBuckets()`, `export()` |
| `Kislay\Metrics\PrometheusExporter` | `registerEndpoint(object $app, Metrics $metrics, string $path = '/metrics', string $namespace = '')` (static), or construct directly and call `export()` |
