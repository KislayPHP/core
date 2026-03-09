# Service Client

`Kislay\ServiceClient` is a lightweight HTTP client for service-to-service calls. It integrates with the Discovery extension to resolve service addresses by name, and provides a fluent builder for headers, timeouts, and retries.

---

## Basic Usage

```php
<?php
$client = new Kislay\ServiceClient('order-service');

// GET
$response = $client->get('/orders');

// POST with body
$response = $client->post('/orders', [
    'product_id' => 42,
    'quantity'   => 3,
]);

// PUT / PATCH / DELETE
$response = $client->put('/orders/{id}', ['status' => 'shipped'], ['id' => 99]);
$response = $client->patch('/orders/{id}', ['note' => 'fragile'], ['id' => 99]);
$response = $client->delete('/orders/{id}', ['id' => 99]);
```

---

## Path Templates

Curly-brace segments in paths are replaced with values from the third argument:

```php
$response = $client->get('/users/{userId}/orders/{orderId}', [], [
    'userId'  => 5,
    'orderId' => 101,
]);
// → GET /users/5/orders/101
```

---

## Builder Methods

```php
$response = $client
    ->withHeader('X-Correlation-Id', $correlationId)
    ->withTimeout(5)          // seconds
    ->withRetry(3, 200)       // 3 retries, 200 ms back-off
    ->get('/orders');
```

---

## Discovery Integration

```php
// Resolve via the Discovery extension automatically
$client = Kislay\ServiceClient::fromDiscovery('payment-service', $discovery);
$response = $client->post('/payments', ['amount' => 999]);
```

---

## Response Format

Every call returns an associative array:

```php
[
  'status'  => 200,
  'headers' => ['Content-Type' => 'application/json'],
  'body'    => ['id' => 123, 'status' => 'ok'],   // parsed JSON or raw string
  'ok'      => true,   // true when status < 400
]
```

---

## Error Handling

```php
$response = $client->get('/orders');

if (!$response['ok']) {
    throw new RuntimeException(
        "order-service error {$response['status']}: " . json_encode($response['body'])
    );
}
```
