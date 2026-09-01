# EventBus (WebSocket)

## Overview

`kislayphp/eventbus` embeds a Socket.IO-compatible WebSocket server inside your KislayPHP process. Clients connect using any Socket.IO client library; the server handles namespaces, rooms, acknowledgements, and JWT-based connection authentication — all with the same C++ event loop used by the HTTP core.

## Installation

There are no pre-built binaries yet — this compiles from source via [PIE](https://github.com/php/pie) or a manual `phpize` build, the same as core. See [Installation](../getting-started/installation.md). Note: `eventbus` is a compatibility package for the older transport name — new work should generally use `kislayphp/socket` instead (see its own docs); both share the same `Kislay\EventBus\*` / civetweb constraints described here.

GitHub: <https://github.com/KislayPHP/eventbus>

---

## Key Features

- **Socket.IO protocol** — compatible with socket.io-client v4+
- **Namespaces** — isolate event streams (e.g. `/chat`, `/notifications`)
- **Rooms** — broadcast to subsets of connected clients
- **JWT authentication** — validate token on handshake before accepting connection
- **Acknowledgements (ACK)** — request/response pattern over WebSocket
- **EventBus bridge** — internal PHP events propagate to connected clients
- **Horizontal scale** — Redis adapter for multi-node pub/sub (optional)

---

## Quick Example

Event handlers are registered once at the server (or namespace) level with
`on()`/`onWithAck()` — they fire for *any* client that emits that event,
they are not registered per-connected-socket:

```php
<?php
$server = new Kislay\EventBus\Server();

$server->on('connection', function (Kislay\EventBus\Socket $socket) {
    $socket->join('general');
});

$server->on('chat', function (Kislay\EventBus\Socket $socket, array $payload) {
    $socket->emitTo('general', 'chat', [
        'from'    => $socket->id(),
        'message' => $payload['text'] ?? '',
    ]);
});

$server->setThreads(4);
$server->listen('0.0.0.0', 8081);
```

`onWithAck()` works like `on()`, but the handler's return value is sent
back to the caller as the acknowledgement. `namespace(string $name)`
returns a `Kislay\EventBus\EventNamespace` exposing its own scoped `on()`/
`emit()`/`emitTo()` — it does not take a second options argument; there is
no built-in per-namespace `requireAuth` option.

---

## Configuration

| Method | Signature | Description |
|---|---|---|
| `on` / `onWithAck` | `(string $event, callable $handler): bool` | Register a global event handler |
| `emit` / `publish` / `send` | `(string $event, mixed $data): bool` | Broadcast to all connected clients |
| `emitTo` | `(string $room, string $event, mixed $data): bool` | Broadcast to one room |
| `namespace` | `(string $name): EventNamespace` | Scoped handler/emit subset |
| `setThreads` | `(int $count)` | CivetWeb worker thread count, before `listen()` |
| `setMaxPayload` | `(int $bytes)` | Max WS frame size |
| `onAuth` | `(callable $handler)` | Hook to validate the handshake before accepting a connection |

See [eventbus's own README](https://github.com/KislayPHP/eventbus) for the full, verified API reference — it's kept in sync with the source more closely than this page.
