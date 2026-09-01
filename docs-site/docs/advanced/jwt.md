# JWT Security

KislayPHP Core includes built-in JWT validation backed by a C++ HS256 verifier (RS256 is not currently implemented). When enabled, every request must carry a valid Bearer token; the decoded payload is attached to `$req->user()`.

---

## Enabling JWT

```php
<?php
$app = new Kislay\Core\App();

// HS256 shared secret
$app->setOption('jwt_secret',   getenv('JWT_SECRET'));
$app->setOption('jwt_required', true);

// Optional: exclude certain paths from JWT enforcement
$app->setOption('jwt_exclude', ['/health', '/actuator', '/public']);

$app->listen('0.0.0.0', 8080);
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

## Options Reference

Only three JWT-related `setOption()` keys actually exist in the current
source (verified against every `key == "..."` branch in
`App::setOption()`):

| Option | Type | Description |
|---|---|---|
| `jwt_secret` | string | HS256 shared secret |
| `jwt_required` | bool | Enforce JWT on all routes |
| `jwt_exclude` | array | Path prefixes exempt from JWT |

There is currently **no RS256/public-key support and no `iss`/`aud`
claim validation** — `jwt_algorithm`, `jwt_public_key`, `jwt_issuer`, and
`jwt_audience` are not implemented; calling `setOption()` with any of
them emits `E_WARNING: Unsupported option` and does nothing. Only HS256
via `jwt_secret` works today.
