# JWT Security

KislayPHP Core includes built-in JWT validation backed by a C++ HS256/RS256 verifier. When enabled, every request must carry a valid Bearer token; the decoded payload is attached to `$req->user()`.

---

## Enabling JWT

```php
<?php
$app = new Kislay\App();

// HS256 shared secret
$app->setOption('jwt_secret',   getenv('JWT_SECRET'));
$app->setOption('jwt_required', true);

// Optional: exclude certain paths from JWT enforcement
$app->setOption('jwt_exclude', ['/health', '/actuator', '/public']);

$app->listen();
```

---

## Accessing the User

```php
$app->get('/profile', function ($req, $res) {
    $user = $req->user();       // decoded JWT payload (array)
    $sub  = $user['sub'];      // subject claim
    $res->json(['userId' => $sub]);
});
```

---

## Role-based Access

```php
$app->get('/admin', function ($req, $res) {
    if (!$req->hasRole('admin')) {
        return $res->status(403)->json(['error' => 'Forbidden']);
    }
    $res->json(['secret' => 'data']);
});
```

`$req->hasRole()` checks the `roles` array claim in the token payload.

---

## RS256 (Public Key)

```php
$app->setOption('jwt_algorithm', 'RS256');
$app->setOption('jwt_public_key', file_get_contents('/etc/keys/public.pem'));
```

---

## Options Reference

| Option | Type | Description |
|---|---|---|
| `jwt_secret` | string | HS256 shared secret |
| `jwt_required` | bool | Enforce JWT on all routes |
| `jwt_exclude` | array | Path prefixes exempt from JWT |
| `jwt_algorithm` | string | `HS256` (default) or `RS256` |
| `jwt_public_key` | string | PEM public key for RS256 |
| `jwt_issuer` | string | Validate `iss` claim if set |
| `jwt_audience` | string | Validate `aud` claim if set |
