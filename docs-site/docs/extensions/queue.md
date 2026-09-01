# Message Queue

## Overview

`kislayphp/queue` is a native distributed job queue: a standalone queue server process, producer/worker clients, retries with backoff, delayed jobs, and dead-letter queue (DLQ) support. Delivery is at-least-once, with one leased job per worker fetch. Server state is in-memory only (no durable persistence backend — the RocksDB/Redis persistence options described in older docs don't exist).

A separate, legacy `Kislay\Queue\Queue` class also exists as an in-process, single-node queue for local development — it's not the recommended path for real use.

## Installation

```bash
pie install kislayphp/queue:1.0.0
```

```ini
extension=kislayphp_queue.so
```

GitHub: <https://github.com/KislayPHP/queue>

---

## Key Features

- **Standalone server** (`Kislay\Queue\Server`) — owns queue state, leases jobs, handles retries/DLQ
- **Producer client** (`Kislay\Queue\Client`) — `push()`/`pushBatch()`, `stats()`, `purge()`
- **Worker client** (`Kislay\Queue\Worker`) — `consume()` with a handler callback
- **Job control** (`Kislay\Queue\Job`) — `ack()`/`nack()`/`release()`, attempt tracking
- **Delayed delivery, TTL, priority via `declare()`/`push()` options**
- **Dead-letter queue** — configured per-queue via `declare()`

---

## Quick Example

Start the queue server (its own process):

```php
<?php
$server = new Kislay\Queue\Server();
$server->declare('emails', [
    'visibility_timeout_ms' => 30000,
    'max_attempts' => 5,
    'retry_backoff_ms' => 1000,
    'dead_letter_queue' => 'emails.dlq',
]);
$server->listen('0.0.0.0', 9020);
$server->run();
```

Push a job (producer):

```php
<?php
$client = new Kislay\Queue\Client('http://127.0.0.1:9020');

$jobId = $client->push('emails', [
    'to' => 'user@example.com',
    'subject' => 'Welcome',
], [
    'headers' => ['trace_id' => 'trace-1'],
    'max_attempts' => 5,
]);
```

Run a worker:

```php
<?php
$worker = new Kislay\Queue\Worker('http://127.0.0.1:9020');

$worker->consume('emails', function (Kislay\Queue\Job $job) {
    $payload = $job->payload();
    sendEmail($payload['to'], $payload['subject']);
    return true; // truthy ack; return false or throw to nack
}, [
    'worker_id' => 'emails-worker-1',
    'lease_ms' => 30000,
]);
```

---

## Public API

```php
namespace Kislay\Queue;

class Server {
    public function __construct(array $options = []);
    public function listen(string $host, int $port): bool;
    public function run(): void;
    public function stop(): bool;
    public function declare(string $queue, ?array $options = null): bool;
    public function stats(?string $queue = null): array;
}

class Client {
    public function __construct(string $baseUrl, array $options = []);
    public function push(string $queue, mixed $payload, ?array $options = null): string;
    public function pushBatch(string $queue, array $jobs): array;
    public function stats(string $queue): array;
    public function purge(string $queue): int;
}

class Worker {
    public function __construct(string $baseUrl, array $options = []);
    public function consume(string $queue, callable $handler, ?array $options = null): bool;
    public function stop(): bool;
}

class Job {
    public function id(): string;
    public function queue(): string;
    public function payload(): mixed;
    public function headers(): array;
    public function attempts(): int;
    public function maxAttempts(): int;
    public function ack(): bool;
    public function nack(?bool $requeue = true, ?int $delayMs = null): bool;
    public function release(?int $delayMs = null): bool;
}
```

See the [module README](https://github.com/KislayPHP/queue) for the full architecture diagram and the legacy in-process `Queue` class API.
