# Architecture: Why Separate Processes?

If you're coming from Laravel or Symfony, the biggest adjustment isn't a
syntax difference — it's that a KislayPHP deployment is normally **more
than one PHP process**, and three of the extensions specifically **must
never be loaded into the same process together**. This page explains why,
so it reads as an intentional microservices topology instead of a
confusing limitation.

## The rule

> Never load `core`, `gateway`, and `socket` in the same PHP process
> (i.e. never combine more than one of their `extension=` lines in one
> `php.ini` / one `-d extension=` invocation).

## Why: they all vendor their own civetweb

`core`, `gateway`, and `socket` each embed their own compiled copy of
[civetweb](https://github.com/civetweb/civetweb), the multi-threaded C
HTTP server they're all built on, and each exports the same non-static
C symbols (`mg_start` and friends). On platforms that link PHP extensions
with `-flat_namespace` — notably macOS — loading two or more of these
extensions into one process risks one extension's compiled civetweb code
silently shadowing another's, with **no error, no warning** — just
undefined behavior up to and including crashes. This isn't a bug that's
being fixed later; it's a structural consequence of three extensions each
vendoring their own copy of the same C library, and it's why the rule
exists.

## What this actually looks like in practice

This maps naturally onto a microservices topology, which is the point —
you're not working around a limitation, you're expressing the same
separation you'd want anyway:

```
                    ┌─────────────┐
   client ────────▶ │   gateway   │   (one process: routing, JWT/auth,
                    │  (process)  │    rate limiting, circuit breaking)
                    └──────┬──────┘
                           │ proxies to
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │   core   │ │   core   │ │  socket  │
        │(process) │ │(process) │ │(process) │
        │your app  │ │your app  │ │realtime/ │
        │  logic   │ │  logic   │ │WebSocket │
        └──────────┘ └──────────┘ └──────────┘
```

- **`gateway`** runs as its own process at the edge — the thing clients
  actually connect to. It never runs your application code.
- **`core`** runs as one process per backend service — this is where
  your routes/controllers/middleware live. Run as many `core` processes
  as you have backend services (or instances of one service).
- **`socket`** runs as its own process for realtime/WebSocket traffic,
  fronted by `gateway` like any other backend, or run standalone.

None of `eventbus`, `persistence`, `discovery`, `queue`, `metrics`, or
`config` are civetweb-embedding HTTP servers in this sense — `eventbus`
is `socket`'s predecessor (same constraint applies, don't run both
`socket` and `eventbus` alongside `core`/`gateway` either), and the rest
are in-process PHP libraries/clients you `require`/`use` from inside a
`core` (or any) process without restriction. `discovery` and `queue` do
have their own optional standalone server modes (a registry process, a
queue-broker process) — those aren't civetweb-based and don't carry the
same restriction, run them as their own processes for the same reason
you'd run any other broker/registry as its own service.

## Why not just fix it?

A real fix means either de-vendoring civetweb into one shared library all
three extensions link against (a nontrivial ABI/versioning project) or
giving each extension's copy unique symbol names (a vendoring/build
change across three independent codebases). Neither is planned near-term
— current effort is going into stability (sanitizer-verified builds) and
PHP-developer onboarding rather than this kind of internal refactor. If
you're evaluating KislayPHP for production, plan your deployment around
one extension's civetweb server per process from the start; it costs
nothing once it's your default topology.
