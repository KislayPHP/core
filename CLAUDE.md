# kislayphp/core — notes for AI assistants

The HTTP runtime for the KislayPHP ecosystem: embedded HTTP/HTTPS server
(two backends — CivetWeb, the default/stable one, and an experimental
libuv backend under `src/runtime/uv_server.cpp` for higher concurrency),
strict segment router, request/response lifecycle, middleware, an async
bridge (`AsyncHttp`/`Promise`), and worker pooling
(`worker_pool.cpp`/`php_runtime.cpp`). Everything else in the ecosystem
(`gateway`, `socket`, `eventbus`, `persistence`, `discovery`, `queue`,
`metrics`, `config`) builds on this or runs alongside it.

This module has by far the deepest, most subtle bug history in the
ecosystem — almost every fix here was a genuine lifecycle/concurrency bug
in the libuv backend (leaks, use-after-free, non-virtual-dispatch
silently-dropped calls, dangling pointers), several only reproducible
under real concurrent load or only on Linux, not macOS. **If you're
touching `src/runtime/`, budget real empirical testing time, not just code
review** — see "How bugs get found here" below.

## THE ONE STILL-OPEN, UNRESOLVED ISSUE — read this before using `listenAsync()`

**`listenAsync()` + `AsyncHttp`/`Promise` together crash with a SIGBUS,
Darwin-only, 100% reproducible on the very first `AsyncHttp::executeAsync()`
cycle in a process — NOT root-caused despite five separate investigation
passes.** Do not enable this combination as production-safe. Confirmed
NOT to reproduce on Linux/arm64 (Docker, 20/20 clean) — this is
specifically about Darwin's TLS/TSRM mechanics, most likely Apple's TLV
thread-local resolution interacting badly with a process's first-ever
second OS thread. `listenAsync()` itself (the hang) is genuinely fixed and
safe; it's specifically the AsyncHttp/Promise machinery's first activation
that's dangerous. The crash is inside `gc_fetch_unused()`
(`zend_gc.c:443`) during `php_request_shutdown()`'s GC pass — the calling
thread's own `zend_gc_globals` base address reads as garbage. Seven+
concrete hypotheses have been tested and disproven (TSRM staleness,
concurrent-locking, closure/thread-stop ordering, Promise/AsyncHttp's own
refcounting — proved clean via direct instrumentation — full_tables_cleanup,
main-thread cache refresh, TSRM-growth-invalidating-other-threads' cache).
Live lldb single-stepping has never actually happened on this bug despite
four attempted passes — **Developer Mode is disabled on the primary dev
machine**, which hangs `debugserver`/lldb on any process launch; a fifth
pass used macOS crash-reporter `.ips` files as a substitute and got
partial register-level evidence, but a real fix needs `sudo
/usr/sbin/DevToolsSecurity -enable` run first, then real interactive
lldb work. **A safe-ish mitigation if you need this today:** make one
throwaway `executeAsync()` call (any harmless endpoint, callbacks
optional) immediately after `listenAsync()` returns, before any real one —
this reliably avoids the crash in every variant tested, though the
mechanism isn't understood well enough to call it a proven fix.

## How bugs get found here (pattern, not just history)

Several real bugs in this module were found only through hands-on
empirical testing — writing a small repro script, running it repeatedly,
and watching for the failure — not by reading code alone:
- The libuv `process_responses()` UAF (fixed): a `UvConnection*` could be
  dereferenced after `on_close()` already deleted it, under concurrent
  resets. Fixed via a `live_connections_` registry checked before any
  dereference.
- The dangling `llhttp` settings pointer (fixed): `llhttp_init()` only
  stores a pointer to the settings struct, which was stack-local — every
  libuv connection ever had a dangling `parser->settings`. "Worked" on
  macOS purely by luck (stack reuse timing); 100% SIGSEGV on first request
  on Linux/arm64. Found while building Linux/Valgrind tooling for the
  listenAsync SIGBUS above (which itself does NOT reproduce on Linux).
- `RequestCompletion::complete()` not being `virtual` (fixed): a derived
  `UvCompletion`'s override looked like an override but was plain C++
  name-hiding — calls through the base-typed pointer silently invoked the
  wrong (CivetWeb-flavored) implementation, meaning libuv responses were
  computed correctly and then just discarded. No crash, no error — just
  silently wrong behavior, only found by tracing the call chain by hand.
- `async()`/`AsyncHttp` and `sendFile()` were **entirely broken** on the
  libuv backend (fixed) — `kislay_active_app` was simply never set on
  that path; `sendFile()` had no file-body support implemented at all.
- `uv_write()`'s return value being discarded (fixed 2026-08-30): on a
  synchronous write failure, libuv's `on_write_done` callback — the only
  place that frees the response buffer and the `WriteReq` — never fires,
  leaking both. Reachable via a real race (`uv_close()` marks a handle
  closing immediately; the matching cleanup that would otherwise catch
  this runs on a later loop tick), not just a theoretical code-path.

**If you add new libuv-backend code, write a real concurrent-load test
before considering it done** — see `core/tests/` for the load-test pattern
(`core_libuv_concurrency_load_test`-style: real concurrent requests, not
just single-request phpt cases) added specifically because
single-request tests kept missing bugs that only manifest under
concurrent resets/connection churn.

## Known, deliberately-not-fixed limitation

`WorkerPool::stop()`/`PhpRuntimePool::stop()` silently drop thread-local
pending retries/queued requests on shutdown (audited 2026-08-30). Not a
crash and not an unbounded leak (bounded to one App's lifetime, no Zend
refs involved — `HttpRequestTask`/`RuntimeRequestMessage` hold no zvals) —
but a `Promise` registered for a dropped retry never resolves/rejects, and
a caller in `RequestCompletion::wait_for()` times out instead of getting
an immediate response. Left unfixed deliberately: a proper fix means
touching `PromiseRegistry` semantics, judged too large a change to make
confidently without a clearer reachable-impact case. Worth a dedicated
session if this needs closing.

## Testing

Standard phpt, `make test`. 20/22 (2 ZTS-only tests correctly skipped on
an NTS build; 0 failures) as of 2026-08-30. For libuv-specific work, also
build/test against real ZTS (`./build_zts_php.sh`) — several bugs here
only manifest under ZTS or under Linux, not this machine's default NTS
macOS build; see `Dockerfile.zts-sigbus` for a reusable Linux/arm64 debug
ZTS environment (Docker Desktop on Apple Silicon runs it natively, no
emulation — a genuinely useful "same CPU, different OS" comparison point).

## Known open issues

1. `listenAsync()` + `AsyncHttp`/`Promise` SIGBUS — see above, the
   headline open issue in this entire ecosystem as of 2026-08-30.
2. `WorkerPool`/`PhpRuntimePool` shutdown silently drops pending
   work/retries — see above, deliberately deferred, not urgent.
