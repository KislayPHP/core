# EventBus (WebSocket)

## Overview

`kislayphp/eventbus` embeds a Socket.IO-compatible WebSocket server inside your KislayPHP process. Clients connect using any Socket.IO client library; the server handles namespaces, rooms, acknowledgements, and JWT-based connection authentication — all with the same C++ event loop used by the HTTP core.

## Installation

```bash
composer require kislayphp/eventbus
```

GitHub: <https://github.com/KislayPHP/php-kislay-eventbus>

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

```php
<?php
$app      = new Kislay\App();
$eventbus = new Kislay\EventBus($app);

// Require JWT on the /secure namespace
$eventbus->namespace('/secure', ['requireAuth' => true]);

// Listen for events on the default namespace
$eventbus->on('connection', function ($socket) use ($eventbus) {
    echo "Client connected: {$socket->id}\n";

    $socket->on('chat:message', function ($data, $ack) use ($socket, $eventbus) {
        // Broadcast to everyone in room
        $eventbus->to($data['room'])->emit('chat:message', [
            'from'    => $socket->id,
            'message' => $data['text'],
        ]);
        // Acknowledge sender
        $ack(['delivered' => true]);
    });

    $socket->on('join', function ($room) use ($socket) {
        $socket->join($room);
        $socket->emit('joined', ['room' => $room]);
    });

    $socket->on('disconnect', function () use ($socket) {
        echo "Client disconnected: {$socket->id}\n";
    });
});

$app->listen();
```

---

## Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| `path` | string | `/socket.io` | Socket.IO handshake path |
| `requireAuth` | bool | `false` | Validate JWT on connection |
| `ping_interval` | int | `25` | Heartbeat interval (s) |
| `ping_timeout` | int | `20` | Client timeout (s) |
| `max_payload` | int | `1048576` | Max WS frame bytes (1 MB) |
| `redis_adapter` | string | — | Redis URL for multi-node pub/sub |
