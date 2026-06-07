<?php
declare(strict_types=1);

namespace Kislay\Core;

/**
 * Lightweight service container for kislayphp.
 *
 * Provides singleton and factory bindings — the PHP-level equivalent of
 * Spring Boot's ApplicationContext / @Autowired ecosystem.
 *
 * @example
 *   Container::singleton(UserRepository::class, fn() => new UserRepository(Config::get('db.dsn')));
 *   Container::singleton(UserService::class,    fn() => new UserService(Container::get(UserRepository::class)));
 *
 *   // Controllers resolved as singletons — no new instance per request
 *   $app->get('/users', [UserController::class, 'index']);
 */
class Container
{
    /** @var array<string, callable> */
    private static array $singletons = [];

    /** @var array<string, callable> */
    private static array $factories = [];

    /** @var array<string, object> */
    private static array $instances = [];

    // ── Registration ─────────────────────────────────────────────────────────

    /**
     * Register a singleton binding.
     * The factory is called once; all subsequent get() calls return the same instance.
     */
    public static function singleton(string $abstract, callable $factory): void
    {
        self::$singletons[$abstract] = $factory;
        unset(self::$instances[$abstract]);
    }

    /**
     * Register a factory binding.
     * A new instance is created on every get() call.
     */
    public static function factory(string $abstract, callable $factory): void
    {
        self::$factories[$abstract] = $factory;
        unset(self::$instances[$abstract]);
    }

    /**
     * Register a pre-built instance directly (always returned as-is).
     */
    public static function instance(string $abstract, object $instance): void
    {
        self::$instances[$abstract] = $instance;
    }

    // ── Resolution ───────────────────────────────────────────────────────────

    /**
     * Resolve a binding.
     *
     * Resolution order:
     *  1. Cached singleton instance
     *  2. Singleton factory (create + cache)
     *  3. Factory (create fresh each time)
     *  4. Auto-wire via PHP Reflection
     *
     * @throws \RuntimeException if the class cannot be resolved
     */
    public static function get(string $abstract): object
    {
        if (isset(self::$instances[$abstract])) {
            return self::$instances[$abstract];
        }

        if (isset(self::$singletons[$abstract])) {
            $instance = (self::$singletons[$abstract])();
            self::$instances[$abstract] = $instance;
            return $instance;
        }

        if (isset(self::$factories[$abstract])) {
            return (self::$factories[$abstract])();
        }

        return self::autoWire($abstract);
    }

    /** Check if a binding (singleton, factory, or cached instance) exists. */
    public static function has(string $abstract): bool
    {
        return isset(self::$singletons[$abstract])
            || isset(self::$factories[$abstract])
            || isset(self::$instances[$abstract]);
    }

    /** Remove a binding and its cached instance. */
    public static function forget(string $abstract): void
    {
        unset(
            self::$singletons[$abstract],
            self::$factories[$abstract],
            self::$instances[$abstract]
        );
    }

    /** Clear all bindings and cached instances. Useful in tests. */
    public static function reset(): void
    {
        self::$singletons = [];
        self::$factories  = [];
        self::$instances  = [];
    }

    // ── Auto-wiring ───────────────────────────────────────────────────────────

    /**
     * Auto-wire a class by resolving constructor parameters via Reflection.
     * Resolved instances are cached as singletons.
     *
     * @throws \RuntimeException
     */
    private static function autoWire(string $class): object
    {
        if (!class_exists($class)) {
            throw new \RuntimeException(
                "Container: cannot resolve '{$class}' — class not found and no binding registered."
            );
        }

        $ref = new \ReflectionClass($class);

        if (!$ref->isInstantiable()) {
            throw new \RuntimeException(
                "Container: '{$class}' is not instantiable (abstract class or interface)."
            );
        }

        $constructor = $ref->getConstructor();

        if ($constructor === null) {
            $instance = $ref->newInstance();
            self::$instances[$class] = $instance;
            return $instance;
        }

        $args = [];
        foreach ($constructor->getParameters() as $param) {
            $type = $param->getType();

            if ($type instanceof \ReflectionNamedType && !$type->isBuiltin()) {
                $args[] = self::get($type->getName());
            } elseif ($param->isDefaultValueAvailable()) {
                $args[] = $param->getDefaultValue();
            } else {
                throw new \RuntimeException(
                    "Container: cannot auto-wire parameter '{$param->getName()}' of '{$class}'"
                    . " — no type hint and no default value."
                );
            }
        }

        $instance = $ref->newInstanceArgs($args);
        self::$instances[$class] = $instance;
        return $instance;
    }
}
