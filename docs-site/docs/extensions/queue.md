# Message Queue

## Overview

`kislayphp/queue` provides an in-process, persistent message queue with first-class support for TTL, dead-letter queues (DLQ), priority levels, and delayed delivery. It is designed to handle background job processing, async task dispatch, and event-driven workflows within a KislayPHP monolith or microservice — no separate broker required for single-node deployments.

## Installation

```bash
composer require kislayphp/queue
```

GitHub: <https://github.com/KislayPHP/php-kislay-queue>

---

## Key Features

- **Enqueue / Dequeue** — push and pop JSON-serialisable payloads
- **Priority queues** — integer priority; higher values processed first
- **Delayed delivery** — schedule messages for future processing
- **TTL** — auto-expire messages that are not consumed in time
- **Dead-letter queue (DLQ)** — failed messages routed to a named DLQ
- **Subscribe** — register a handler that is called for each incoming message
- **Durable persistence** — optional RocksDB or Redis backend

---

## Quick Example

```php
<?php
$queue = new Kislay\Queue();

$queue->connect(['backend' => 'rocksdb', 'path' => '/var/data/queue']);

// Enqueue a standard message
$queue->enqueue('emails', [
    'to'      => 'user@example.com',
    'subject' => 'Welcome!',
]);

// Enqueue with TTL and priority
$queue->enqueue('notifications', ['type' => 'sms', 'body' => 'Code: 1234'], [
    'ttl'      => 60,    // expire in 60 seconds
    'priority' => 10,   // processed before priority < 10
]);

// Delayed message — deliver after 5 minutes
$queue->enqueue('reports', ['report_id' => 42], ['delay' => 300]);

// Subscribe — blocking worker loop
$queue->subscribe('emails', function ($message, $ack) {
    sendEmail($message['to'], $message['subject']);
    $ack(); // acknowledge; remove from queue
});

// Manual dequeue
$msg = $queue->dequeue('notifications');
if ($msg) {
    processNotification($msg);
    $queue->ack($msg['id']);
}
```

---

## Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| `backend` | string | `memory` | `memory`, `rocksdb`, or `redis` |
| `path` | string | `/tmp/kislay-queue` | Data directory (RocksDB) |
| `redis_url` | string | — | Redis connection URL |
| `default_ttl` | int | `0` (no expiry) | Default message TTL (s) |
| `dlq_suffix` | string | `:dlq` | Dead-letter queue name suffix |
| `max_retries` | int | `3` | Retries before DLQ routing |
| `workers` | int | `2` | Subscriber thread count |
