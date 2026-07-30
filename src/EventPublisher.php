<?php
declare(strict_types=1);

namespace Kislay\Core;

/**
 * Typed, in-process event bus — Spring-style ApplicationEventPublisher.
 *
 * Events are plain PHP objects (no interface or base class required).
 * Listeners are matched by the published event's concrete class, every
 * parent class in its hierarchy, and every interface it implements —
 * mirroring Spring's event-hierarchy dispatch so that a listener registered
 * for a base event type receives all subtype events automatically.
 *
 * PHP is single-threaded per request under normal FPM/long-running-process use;
 * the static listener registry is therefore inherently request-safe.
 *
 * @example
 *   // Declare events as plain value objects
 *   class UserRegistered {
 *       public function __construct(public readonly int $userId) {}
 *   }
 *   class AdminUserRegistered extends UserRegistered {}
 *
 *   // Register listeners
 *   EventPublisher::listen(UserRegistered::class, function (UserRegistered $e) {
 *       // fires for both UserRegistered and AdminUserRegistered events
 *       sendWelcomeEmail($e->userId);
 *   });
 *
 *   EventPublisher::listenOnce(AdminUserRegistered::class, function (AdminUserRegistered $e) {
 *       // fires exactly once, then automatically unregistered
 *       notifySecurityTeam($e->userId);
 *   });
 *
 *   // Publish
 *   EventPublisher::publish(new AdminUserRegistered(42));
 */
class EventPublisher
{
    /**
     * Persistent listeners — survive repeated publish() calls.
     *
     * @var array<string, callable[]>
     */
    private static array $listeners = [];

    /**
     * One-shot listeners — removed immediately after their first invocation.
     *
     * @var array<string, callable[]>
     */
    private static array $onceListeners = [];

    // ── Registration ──────────────────────────────────────────────────────────

    /**
     * Register a persistent listener for the given event class.
     *
     * The listener is invoked every time a matching event is published.
     * Multiple listeners per event class are supported and called in
     * registration order.
     *
     * @param string   $eventClass Fully-qualified class name of the event
     * @param callable $listener   fn(EventType $event): void
     */
    public static function listen(string $eventClass, callable $listener): void
    {
        self::$listeners[$eventClass][] = $listener;
    }

    /**
     * Register a one-shot listener for the given event class.
     *
     * The listener is called at most once — on the first matching publish() —
     * and is automatically removed afterwards.  Useful for "wait for X to
     * happen, then do Y exactly once" patterns.
     *
     * @param string   $eventClass Fully-qualified class name of the event
     * @param callable $listener   fn(EventType $event): void
     */
    public static function listenOnce(string $eventClass, callable $listener): void
    {
        self::$onceListeners[$eventClass][] = $listener;
    }

    // ── Dispatch ──────────────────────────────────────────────────────────────

    /**
     * Publish an event to all matching listeners.
     *
     * Resolution order per type in the hierarchy:
     *  1. Persistent listeners (in registration order)
     *  2. One-shot listeners   (in registration order, then atomically removed)
     *
     * The hierarchy is walked as: concrete class → parent classes (nearest
     * first via get_parent_class()) → implemented interfaces (in PHP's
     * class_implements() order).
     *
     * Listeners are dispatched synchronously.  Any exception thrown by a
     * listener propagates immediately to the caller; subsequent listeners for
     * the same event are not invoked.
     *
     * @param object $event The event object to dispatch
     */
    public static function publish(object $event): void
    {
        foreach (self::resolveHierarchy($event) as $type) {
            // --- Persistent listeners ---
            foreach (self::$listeners[$type] ?? [] as $listener) {
                $listener($event);
            }

            // --- One-shot listeners ---
            // Grab and clear atomically before invoking so that a listener
            // which itself calls publish() for the same event type won't see
            // these entries again.
            if (!empty(self::$onceListeners[$type])) {
                $shots = self::$onceListeners[$type];
                unset(self::$onceListeners[$type]);
                foreach ($shots as $listener) {
                    $listener($event);
                }
            }
        }
    }

    // ── Management ────────────────────────────────────────────────────────────

    /**
     * Remove all listeners (persistent and one-shot) registered for $eventClass.
     *
     * Does not affect listeners registered for parent or child event classes.
     *
     * @param string $eventClass Fully-qualified class name of the event
     */
    public static function forget(string $eventClass): void
    {
        unset(self::$listeners[$eventClass], self::$onceListeners[$eventClass]);
    }

    /**
     * Remove all listeners for every event class.
     *
     * Intended for use in test tear-down to prevent listener bleed between tests.
     */
    public static function reset(): void
    {
        self::$listeners     = [];
        self::$onceListeners = [];
    }

    // ── Inspection (test/debug helpers) ───────────────────────────────────────

    /**
     * Return the number of persistent listeners registered for $eventClass.
     *
     * Does not count inherited listeners (e.g. those registered for a parent class).
     */
    public static function listenerCount(string $eventClass): int
    {
        return count(self::$listeners[$eventClass] ?? []);
    }

    /**
     * Return true if at least one listener (persistent or once) is registered
     * that would fire when $eventClass is published.
     *
     * Walks the full hierarchy just like publish() does.
     */
    public static function hasListeners(string $eventClass): bool
    {
        if (!class_exists($eventClass) && !interface_exists($eventClass)) {
            return !empty(self::$listeners[$eventClass])
                || !empty(self::$onceListeners[$eventClass]);
        }

        // Use a throw-away instance to walk the hierarchy
        // Without constructing the event, we inspect class metadata directly.
        $types = self::resolveClassHierarchy($eventClass);
        foreach ($types as $type) {
            if (!empty(self::$listeners[$type]) || !empty(self::$onceListeners[$type])) {
                return true;
            }
        }
        return false;
    }

    // ── Internals ─────────────────────────────────────────────────────────────

    /**
     * Build the ordered list of types to check for an event object:
     * concrete class → parent classes (nearest ancestor first) → interfaces.
     *
     * @return string[]
     */
    private static function resolveHierarchy(object $event): array
    {
        return self::resolveClassHierarchy(get_class($event));
    }

    /**
     * Build the ordered type hierarchy for a class name string.
     *
     * @return string[]
     */
    private static function resolveClassHierarchy(string $class): array
    {
        $types = [];

        // Walk parent chain — get_class() already gives us the concrete class
        $current = $class;
        while ($current !== false) {
            $types[] = $current;
            $current = get_parent_class($current);
        }

        // Append all interfaces (class_implements returns a flat array keyed by name)
        foreach (array_keys(class_implements($class) ?: []) as $iface) {
            $types[] = $iface;
        }

        return $types;
    }
}
