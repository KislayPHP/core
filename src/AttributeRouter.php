<?php
declare(strict_types=1);

namespace Kislay\Core;

// ── Route Attributes ──────────────────────────────────────────────────────────

/**
 * Generic route attribute — place on a controller method to declare any HTTP verb.
 *
 * @example
 *   #[Route('/users', 'POST')]
 *   public function create(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Route
{
    public function __construct(
        public readonly string $path,
        public readonly string $method = 'GET'
    ) {}
}

/**
 * Convenience alias for HTTP GET routes.
 *
 * @example
 *   #[Get('/users')]
 *   public function index(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Get
{
    public function __construct(public readonly string $path) {}
}

/**
 * Convenience alias for HTTP POST routes.
 *
 * @example
 *   #[Post('/users')]
 *   public function store(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Post
{
    public function __construct(public readonly string $path) {}
}

/**
 * Convenience alias for HTTP PUT routes.
 *
 * @example
 *   #[Put('/users/{id}')]
 *   public function replace(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Put
{
    public function __construct(public readonly string $path) {}
}

/**
 * Convenience alias for HTTP PATCH routes.
 *
 * @example
 *   #[Patch('/users/{id}')]
 *   public function update(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Patch
{
    public function __construct(public readonly string $path) {}
}

/**
 * Convenience alias for HTTP DELETE routes.
 *
 * @example
 *   #[Delete('/users/{id}')]
 *   public function destroy(Request $req, Response $res): void { ... }
 */
#[\Attribute(\Attribute::TARGET_METHOD | \Attribute::IS_REPEATABLE)]
class Delete
{
    public function __construct(public readonly string $path) {}
}

// ── Scanner ───────────────────────────────────────────────────────────────────

/**
 * PHP 8 Attribute-driven route scanner.
 *
 * Scans controller classes for #[Route], #[Get], #[Post], #[Put], #[Patch], and
 * #[Delete] attributes on their public methods and registers each discovered route
 * on a {@see App} instance.  Controllers are resolved as singletons via
 * {@see Container::get()}, so the same controller instance handles every request.
 *
 * @example
 *   class UserController {
 *       #[Get('/users')]
 *       public function index(Request $req, Response $res): void { ... }
 *
 *       #[Post('/users')]
 *       public function store(Request $req, Response $res): void { ... }
 *
 *       #[Get('/users/{id}')]
 *       #[Route('/users/{id}', 'HEAD')]
 *       public function show(Request $req, Response $res): void { ... }
 *   }
 *
 *   AttributeRouter::register($app, UserController::class);
 *   // or pass multiple classes at once:
 *   AttributeRouter::register($app, [UserController::class, OrderController::class]);
 */
class AttributeRouter
{
    /**
     * Scan one or more controller classes for route attributes and register
     * the discovered routes with $app.
     *
     * @param App             $app     Application instance to register routes on
     * @param string|string[] $classes One or more controller class FQCNs
     *
     * @throws \InvalidArgumentException if a supplied class does not exist
     * @throws \RuntimeException         if a #[Route] attribute specifies an unsupported HTTP verb
     */
    public static function register(App $app, string|array $classes): void
    {
        foreach ((array) $classes as $class) {
            if (!class_exists($class)) {
                throw new \InvalidArgumentException(
                    "AttributeRouter: class '{$class}' does not exist."
                );
            }

            $ref = new \ReflectionClass($class);

            foreach ($ref->getMethods(\ReflectionMethod::IS_PUBLIC) as $method) {
                // Skip inherited Object methods to avoid noise
                if ($method->getDeclaringClass()->getName() === $class) {
                    self::processMethod($app, $class, $method);
                }
            }
        }
    }

    // ── Internals ─────────────────────────────────────────────────────────────

    /**
     * Collect every route attribute from $method and bind each to $app.
     */
    private static function processMethod(App $app, string $class, \ReflectionMethod $method): void
    {
        foreach (self::collectRouteAttributes($method) as [$path, $verb]) {
            $handler = self::buildHandler($class, $method->getName());
            self::bindRoute($app, $verb, $path, $handler);
        }
    }

    /**
     * Gather all route definitions declared on $method.
     *
     * A method may carry multiple attributes of any mix of types.
     * Each is returned as a [path, HTTP_VERB] pair.
     *
     * @return array<int, array{string, string}>
     */
    private static function collectRouteAttributes(\ReflectionMethod $method): array
    {
        $routes = [];

        // #[Route(path, method)] — generic form, supports any verb
        foreach ($method->getAttributes(Route::class) as $attr) {
            /** @var Route $route */
            $route    = $attr->newInstance();
            $routes[] = [$route->path, strtoupper($route->method)];
        }

        // Convenience verb-specific aliases
        $verbMap = [
            Get::class    => 'GET',
            Post::class   => 'POST',
            Put::class    => 'PUT',
            Patch::class  => 'PATCH',
            Delete::class => 'DELETE',
        ];

        foreach ($verbMap as $attrClass => $verb) {
            foreach ($method->getAttributes($attrClass) as $attr) {
                /** @var Get|Post|Put|Patch|Delete $instance */
                $instance = $attr->newInstance();
                $routes[] = [$instance->path, $verb];
            }
        }

        return $routes;
    }

    /**
     * Build a lazy handler that resolves the controller singleton via
     * {@see Container::get()} on first invocation and invokes the named method,
     * forwarding all request arguments.
     */
    private static function buildHandler(string $class, string $methodName): callable
    {
        return static function () use ($class, $methodName) {
            $controller = Container::get($class);
            return $controller->$methodName(...func_get_args());
        };
    }

    /**
     * Delegate to the correct routing method on $app for the given HTTP verb.
     *
     * @throws \RuntimeException for unrecognised verbs
     */
    private static function bindRoute(App $app, string $verb, string $path, callable $handler): void
    {
        match ($verb) {
            'GET'    => $app->get($path, $handler),
            'POST'   => $app->post($path, $handler),
            'PUT'    => $app->put($path, $handler),
            'PATCH'  => $app->patch($path, $handler),
            'DELETE' => $app->delete($path, $handler),
            default  => throw new \RuntimeException(
                "AttributeRouter: unsupported HTTP method '{$verb}'. "
                . "Supported: GET, POST, PUT, PATCH, DELETE."
            ),
        };
    }
}
