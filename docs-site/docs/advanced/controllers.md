# Controller Syntax

KislayPHP supports two equivalent ways to reference a controller method as a route handler: the **Class@method string syntax** and the **[Class, method] array syntax**.

---

## Array Syntax (recommended)

Pass an instantiated controller and method name as a two-element array:

```php
<?php
class UserController
{
    public function index($req, $res): void
    {
        $res->json(['users' => []]);
    }

    public function show($req, $res): void
    {
        $res->json(['id' => $req->params['id']]);
    }

    public function store($req, $res): void
    {
        $res->status(201)->json(['created' => true]);
    }
}

$ctrl = new UserController();

$app->get('/users',      [$ctrl, 'index']);
$app->get('/users/{id}', [$ctrl, 'show']);
$app->post('/users',     [$ctrl, 'store']);
```

---

## String Syntax

Pass a `"ClassName@method"` string; KislayPHP instantiates the class with `new ClassName()`:

```php
$app->get('/users',      'UserController@index');
$app->get('/users/{id}', 'UserController@show');
$app->post('/users',     'UserController@store');
```

!!! note "Instantiation"
    String syntax calls `new ClassName()` with no constructor arguments. Use the array syntax when your controller needs dependencies injected.

---

## Middleware on Controllers

Apply middleware to a controller group using `$app->group()`:

```php
$app->group('/admin', [$authMiddleware], function ($app) {
    $ctrl = new AdminController();
    $app->get('/users',  [$ctrl, 'users']);
    $app->get('/config', [$ctrl, 'config']);
});
```

---

## Full Controller Example

```php
<?php
class OrderController
{
    public function __construct(
        private readonly OrderRepository $repo,
        private readonly Kislay\Metrics  $metrics,
    ) {}

    public function index($req, $res): void
    {
        $page   = (int)($req->query['page'] ?? 1);
        $orders = $this->repo->paginate($page);
        $this->metrics->counter('orders_listed_total')->inc();
        $res->json($orders);
    }
}

$ctrl = new OrderController($repo, $metrics);
$app->get('/orders', [$ctrl, 'index']);
```
