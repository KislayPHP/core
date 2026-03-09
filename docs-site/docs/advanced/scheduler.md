# Task Scheduler

The KislayPHP core extension exposes three scheduling primitives built on the same C++ timer wheel used by the HTTP server — no cron daemon required.

---

## every — Repeating interval

```php
// Run every 5 seconds
$app->every(5000, function () {
    echo "Tick: " . date('H:i:s') . "\n";
});

// Run every minute with a label (for logging)
$app->every(60_000, function () use ($metrics) {
    $metrics->gauge('queue_depth')->set(Queue::depth('emails'));
}, 'queue-depth-probe');
```

---

## once — Delayed one-shot

```php
// Run once after 10 seconds
$app->once(10_000, function () {
    warmUpCaches();
});
```

---

## schedule — Cron expression

```php
// Every hour at :00
$app->schedule('0 * * * *', function () {
    generateHourlyReport();
});

// Every weekday at 09:00
$app->schedule('0 9 * * 1-5', function () {
    sendDailyDigest();
});
```

---

## Cron Syntax

```
┌─ minute       (0-59)
│ ┌─ hour       (0-23)
│ │ ┌─ day      (1-31)
│ │ │ ┌─ month  (1-12)
│ │ │ │ ┌─ weekday (0-7, 0=Sun)
│ │ │ │ │
* * * * *
```

Special: `@hourly`, `@daily`, `@weekly`, `@monthly`, `@yearly`.

---

## Complete Example

```php
<?php
$app     = new Kislay\App();
$metrics = new Kislay\Metrics($app);
$queue   = new Kislay\Queue();

// Heartbeat every 30 s
$app->every(30_000, fn () => $metrics->counter('heartbeat_total')->inc());

// Process scheduled reports daily at midnight
$app->schedule('0 0 * * *', function () use ($queue) {
    $queue->enqueue('reports', ['type' => 'daily', 'date' => date('Y-m-d')]);
});

// Warm cache 2 s after startup
$app->once(2_000, fn () => warmCache());

$app->listen();
```
