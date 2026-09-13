# DB Persistence

## Overview

`kislayphp/persistence` provides per-request database lifecycle management for `kislayphp/core` apps, backed by plain PDO. It supports MySQL/MariaDB, PostgreSQL, and SQLite through a small static `DB` facade with raw SQL + bindings (no fluent query builder), automatic transaction begin/commit/rollback tied to the request lifecycle, and a lightweight PHP-level migration runner.

## Installation

```bash
pie install kislayphp/persistence
```

Enable in `php.ini`:

```ini
extension=kislayphp_persistence.so
```

---

## Key Features

- **Multiple named connections** — `mysql`/`mariadb`, `pgsql`, and `sqlite` drivers, one default plus any number of named connections
- **Automatic per-request transactions** — `Runtime::attach($app)` begins a transaction on the first write in a request and commits on a clean return or rolls back on an uncaught exception
- **Manual transaction control** — a `Runtime` instance (`begin`/`commit`/`rollback`/`isActive`) or `DB::transaction(callable $callback)`
- **Raw SQL + bindings** — `DB::select`/`insert`/`update`/`delete`, each parameterized (SQL-injection-safe) but not a fluent builder
- **Stale-connection recovery** — a cached PDO handle that's gone dead is transparently reconnected rather than surfacing a hard failure
- **Migrations** — a small PHP-level `Migrations` class (`add`/`run`/`status`), not a CLI tool or file-convention

---

## Quick Example

### Automatic transaction per request

```php
<?php
use Kislay\Persistence\DB;
use Kislay\Persistence\Runtime;

DB::boot([
    'default' => 'main',
    'connections' => [
        'main' => [
            'driver'   => 'mysql',
            'host'     => 'db',
            'port'     => 3306,
            'database' => 'myapp',
            'username' => 'root',
            'password' => getenv('DB_PASSWORD'),
        ],
    ],
]);

$app = new Kislay\Core\App();
Runtime::attach($app);   // begin/commit/rollback per request, automatic

$app->post('/api/orders', function ($req, $res) {
    $data = $req->getJson();

    $id = DB::insert(
        'INSERT INTO orders (product_id, qty, email) VALUES (?, ?, ?)',
        [$data['product_id'], $data['qty'], $data['email']]
    );

    $res->json(['order_id' => $id], 201);
    // transaction commits on clean return, rolls back on uncaught exception
});

$app->listen('0.0.0.0', 8080);
```

### Manual transaction

```php
<?php
use Kislay\Persistence\Runtime;
use Kislay\Persistence\DB;

$persistence = new Runtime();
$persistence->begin();

try {
    $userId = DB::insert('INSERT INTO users (name) VALUES (?)', ['Alice']);
    $persistence->commit();
} catch (\Throwable $e) {
    $persistence->rollback();
    throw $e;
}
```

Or the static shortcut:

```php
DB::transaction(function () {
    DB::update('UPDATE accounts SET balance = balance - 100 WHERE id = ?', [1]);
    DB::update('UPDATE accounts SET balance = balance + 100 WHERE id = ?', [2]);
});
```

---

## Migrations

`Kislay\Persistence\Migrations` is a plain PHP object, not a CLI command:

```php
<?php
use Kislay\Persistence\Migrations;

$migrations = new Migrations();
$migrations->add('001_create_users', 'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)');
$migrations->add('002_create_orders', 'CREATE TABLE orders (id INTEGER PRIMARY KEY, product_id INTEGER)');

$migrations->run();      // applies any not-yet-run migrations, in the order added
$migrations->status();   // inspect what's applied
```

---

## Public API

```php
namespace Kislay\Persistence;

class DB {
    public static function boot(array $config): bool;
    public static function connection(?string $name = null);   // returns the PDO for that connection
    public static function connect(?string $name = null);      // alias of connection()
    public static function transaction(callable $callback, ?string $connection = null): mixed;
    public static function select(string $sql, ?array $bindings = null, ?string $connection = null): array;
    public static function insert(string $sql, ?array $bindings = null, ?string $connection = null): int;
    public static function update(string $sql, ?array $bindings = null, ?string $connection = null): int;
    public static function delete(string $sql, ?array $bindings = null, ?string $connection = null): int;
    public static function ping(?string $connection = null): bool;
}

class Runtime {
    public static function attach(Kislay\Core\App $app): void;
    public function begin(?string $connectionName = null): bool;
    public function commit(): bool;
    public function rollback(): bool;
    public function isActive(): bool;
}

class Migrations {
    public function __construct();
    public function add(string $id, string $sql): void;
    public function run(): void;
    public function status(): array;
}
```

---

## `DB::boot()` connection config

```php
DB::boot([
    'default' => 'main',
    'connections' => [
        'main' => [ /* one of the driver shapes below */ ],
    ],
]);
```

| Driver | Required keys | Optional keys (defaults) |
|---|---|---|
| `sqlite` | `database` (file path) | — |
| `mysql` / `mariadb` | `database` | `host` (`127.0.0.1`), `port` (`3306`), `charset` (`utf8mb4`), `username`, `password` |
| `pgsql` / `postgres` / `postgresql` | `database` | `host` (`127.0.0.1`), `port` (`5432`), `username`, `password` |

There is no separate connection-pool configuration (`pool_min`/`pool_max`) — each named connection is a single cached PDO handle, transparently recreated if it goes stale.
