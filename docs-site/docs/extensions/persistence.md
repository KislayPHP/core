# DB Persistence

## Overview

`kislayphp/persistence` provides a thin, high-performance database abstraction layer backed by a C++ connection pool. It supports MySQL, PostgreSQL, and SQLite through a unified API with prepared statements, transactions, and a lightweight migration runner — all without the overhead of a full ORM.

## Installation

```bash
composer require kislayphp/persistence
```

GitHub: <https://github.com/KislayPHP/php-kislay-persistence>

---

## Key Features

- **Connection pool** — configurable min/max pool size, auto-reconnect
- **Query builder** — fluent `select / insert / update / delete`
- **Prepared statements** — automatic parameter binding, SQL injection safe
- **Transactions** — `begin / commit / rollback` with nested savepoints
- **Migrations** — version-stamped SQL files, `up` / `down` support
- **Multiple connections** — register named connections (read replica, analytics)
- **Drivers** — MySQL 8+, PostgreSQL 14+, SQLite 3

---

## Quick Example

```php
<?php
DB::connect('default', [
    'driver'   => 'mysql',
    'host'     => 'db',
    'port'     => 3306,
    'database' => 'myapp',
    'username' => 'root',
    'password' => getenv('DB_PASSWORD'),
    'pool_min' => 2,
    'pool_max' => 10,
]);

// SELECT
$users = DB::select('SELECT * FROM users WHERE active = ?', [1]);

// Fluent INSERT
$id = DB::table('users')->insert([
    'name'       => 'Alice',
    'email'      => 'alice@example.com',
    'created_at' => date('Y-m-d H:i:s'),
]);

// Fluent UPDATE
DB::table('users')
    ->where('id', $id)
    ->update(['name' => 'Alice Smith']);

// DELETE
DB::table('users')->where('id', $id)->delete();

// Transaction
DB::transaction(function () {
    DB::table('accounts')->where('id', 1)->decrement('balance', 100);
    DB::table('accounts')->where('id', 2)->increment('balance', 100);
});
```

---

## Running Migrations

```bash
php artisan db:migrate          # run pending migrations
php artisan db:migrate --down   # rollback last batch
```

Migration files live in `database/migrations/` and follow the naming convention `001_create_users_table.sql`.

---

## Configuration

| Option | Default | Description |
|---|---|---|
| `driver` | `mysql` | `mysql`, `pgsql`, or `sqlite` |
| `host` | `localhost` | DB host |
| `port` | `3306` | DB port |
| `database` | — | Database name |
| `pool_min` | `2` | Minimum pool connections |
| `pool_max` | `10` | Maximum pool connections |
| `timeout` | `5` | Connection timeout (s) |
| `migrations_path` | `database/migrations` | Migration file directory |
