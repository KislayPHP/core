<?php
declare(strict_types=1);

namespace Kislay\Core;

/**
 * Marks a class as a configuration properties holder.
 *
 * Equivalent to Spring Boot's @ConfigurationProperties.
 *
 * @example
 *   #[ConfigurationProperties(prefix: 'database')]
 *   class DatabaseConfig {
 *       public string $host     = 'localhost';
 *       public int    $port     = 3306;
 *       public string $name     = '';
 *       public string $user     = 'root';
 *       public string $password = '';
 *   }
 *
 *   $cfg = ConfigBinder::bind(DatabaseConfig::class);
 *   // $cfg->host is read from Config::get('database.host')
 */
#[\Attribute(\Attribute::TARGET_CLASS)]
class ConfigurationProperties
{
    public function __construct(public readonly string $prefix = '') {}
}

/**
 * Binds configuration values to a typed PHP DTO using the
 * #[ConfigurationProperties] attribute.
 *
 * Resolution order for each property:
 *  1. Custom $resolver callable
 *  2. Kislay\Config\Config::get() (if available)
 *  3. Environment variable (UPPER_SNAKE_CASE of prefix.propertyName)
 *  4. PHP default value declared on the property
 */
class ConfigBinder
{
    /**
     * Bind configuration values to a typed DTO class.
     *
     * @param string        $class    PHP class with #[ConfigurationProperties]
     * @param callable|null $resolver fn(string $key): mixed — custom value source
     *
     * @return object Populated instance of $class
     * @throws \InvalidArgumentException
     */
    public static function bind(string $class, ?callable $resolver = null): object
    {
        if (!class_exists($class)) {
            throw new \InvalidArgumentException("ConfigBinder: class '{$class}' not found.");
        }

        $ref = new \ReflectionClass($class);

        // Discover prefix from attribute
        $prefix = '';
        $attrs = $ref->getAttributes(ConfigurationProperties::class);
        if (!empty($attrs)) {
            $prefix = $attrs[0]->newInstance()->prefix;
        }

        // Build resolver chain
        if ($resolver === null) {
            $resolver = self::buildDefaultResolver();
        }

        $instance = $ref->newInstanceWithoutConstructor();

        foreach ($ref->getProperties(\ReflectionProperty::IS_PUBLIC) as $prop) {
            $key      = $prefix ? "{$prefix}.{$prop->getName()}" : $prop->getName();
            $rawValue = $resolver($key);

            if ($rawValue === null) {
                if ($prop->hasDefaultValue()) {
                    $prop->setValue($instance, $prop->getDefaultValue());
                }
                continue;
            }

            $prop->setValue($instance, self::cast($rawValue, $prop->getType()));
        }

        return $instance;
    }

    // ── Internals ─────────────────────────────────────────────────────────────

    private static function buildDefaultResolver(): callable
    {
        if (class_exists(\Kislay\Config\Config::class)) {
            return static fn(string $key): mixed => \Kislay\Config\Config::get($key, null);
        }

        // Fall back to env vars (DATABASE_HOST from database.host)
        return static function (string $key): mixed {
            $envKey = strtoupper(str_replace(['.', '-'], '_', $key));
            return $_ENV[$envKey] ?? getenv($envKey) ?: null;
        };
    }

    /** Cast a raw config value to the declared property type. */
    private static function cast(mixed $value, ?\ReflectionType $type): mixed
    {
        if (!($type instanceof \ReflectionNamedType)) {
            return $value;
        }

        return match ($type->getName()) {
            'int'    => (int)$value,
            'float'  => (float)$value,
            'bool'   => filter_var($value, FILTER_VALIDATE_BOOLEAN),
            'string' => (string)$value,
            'array'  => is_array($value)
                            ? $value
                            : (json_decode((string)$value, true) ?? []),
            default  => $value,
        };
    }
}
