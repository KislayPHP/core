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

## Sanitizer builds (ASan/UBSan/TSan)

Top-level `Dockerfile.sanitize` (in the `kislayphp/` workspace, not this
repo) builds this extension against a plain NTS PHP with
`-fsanitize=address,undefined` or `-fsanitize=thread` baked into
CXXFLAGS/LDFLAGS, then runs the phpt suite with the matching sanitizer
runtime `LD_PRELOAD`ed into the PHP CLI host process (the extension is a
`.so`, so global malloc/free interposition only works if the *host*
process has the runtime loaded, not just the `.so`). Usage:

```sh
docker build -f ../Dockerfile.sanitize --build-arg MODULE=core --build-arg SANITIZER=asan -t kislayphp-sanitize-core-asan ..
docker run --rm --cap-add=SYS_PTRACE kislayphp-sanitize-core-asan
```

**Known environment caveat, not a KislayPHP bug:** on the Ubuntu 22.04
base image, OpenSSL's PKCS#11 engine loading pulls in `libp11-kit`, whose
atexit handler can deadlock in `freelocale()`/`pthread_rwlock_wrlock()`
during process exit — reproduces for a trivial `-r 'exit;'` script with
just the extension loaded, confirmed via `gdb -p <pid> -batch -ex 'thread
apply all bt'` landing entirely in libc/libp11-kit frames, no KislayPHP
frames anywhere. Under ASan/UBSan it's non-deterministic (some invocations
exit cleanly). The test runner (`sanitize-run-tests.sh`) wraps every PHP
invocation in `timeout -s KILL`, so this can't hang the suite, but it does
mean a "PASS" can get masked as an `EXIT-HANG` if the deadlock happens to
fire on an otherwise-successful run — treat `EXIT-HANG` results as
inconclusive, not failures.

**Under TSan specifically (tried 2026-08-31), this caveat gets much worse:**
nearly every test (including a bare `SKIPIF` probe script, before the real
test body even runs) hit `EXIT-HANG`, re-confirmed via a fresh live `gdb`
capture as the exact same `freelocale → libp11-kit → pthread_rwlock_wrlock`
stack, at the same `exit()` call site — not a new/different bug, just hit
far more often, most likely because TSan's own instrumentation overhead
widens whatever race window makes this non-deterministic under ASan.
`OPENSSL_CONF=/dev/null` (suggested below as a possible mitigation) was
tried and did **not** avoid it. Net effect: TSan builds now succeed for
this module (see build-fix note below) and the runtime works once invoked
with `--security-opt seccomp=unconfined` (TSan's ASLR-disabling
`personality()` call is blocked by Docker's default seccomp profile
otherwise), but essentially no pass/fail signal could be extracted from a
TSan run in this container environment — it needs a non-glibc base image
(e.g. Alpine/musl) or an OpenSSL build without PKCS#11/engine support
before TSan coverage here is actually usable, not just buildable.

**TSan build fix (2026-08-31, `Dockerfile.sanitize`):** two separate
problems, both specific to `-fsanitize=thread` on aarch64 under Docker,
had to be fixed before the image would even build:
1. TSan's runtime calls `personality(ADDR_NO_RANDOMIZE)` on every process
   start to fix its shadow-memory layout; Docker's default seccomp profile
   blocks that syscall outright, which used to crash even `./configure`'s
   own generic "checking whether we are cross compiling" conftest
   (`configure: error: cannot run C compiled programs`, exit 77) — nothing
   to do with this module's own code.
2. Fix: pass `--host=$(uname -m)-pc-linux-gnu` to `./configure` when
   building with TSan. That triplet is textually different from what
   `config.guess` detects, so autoconf treats the build as cross-compiling
   and skips executing `AC_TRY_RUN` conftests (assumes safe defaults
   instead), avoiding the crash — while `CFLAGS`/`CXXFLAGS`/`LDFLAGS` are
   still exported *before* `./configure` runs (required separately, since
   this module's own `config.m4` does `CFLAGS="$CFLAGS -DOPENSSL_API_3_0
   ..."`, i.e. appends to whatever was already exported — dropping that
   define is what broke civetweb's OpenSSL glue with unrelated-looking
   `'SSL_connect' undeclared` errors during an earlier attempt at this fix).
   At actual `docker run` time, the same `personality()` block still
   applies to the extension's own TSan-instrumented code, so the run
   command needs `--security-opt seccomp=unconfined` too (a normal runtime
   flag — no BuildKit entitlement needed, unlike a build-time workaround).

**Real bug found and fixed this way (2026-08-31):** `app->entry_script_path`
(a `std::string` member of `php_kislay_app_t`) was the only string member
missing from both `kislay_app_create_object`'s placement-new list and
`kislay_app_free_obj`'s placement-destructor list — it "worked" by
accident (zeroed raw memory happens to behave like an empty string on this
libstdc++ ABI) but leaked its heap buffer on every `listen()`/
`listenAsync()` call that ever assigned a real path into it. Fixed by
adding the missing placement-new/destructor pair. Confirmed via
LeakSanitizer: present in 4/4 affected phpt tests before the fix, 0/3
manual repro runs after.

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

## Dropped-promise-on-shutdown fix (2026-09-13)

**FIXED:** a `Promise` for a still-pending `async()` PHP task or
`AsyncHttp`/retry HTTP task used to never resolve/reject if the `App` was
destroyed before the task was drained — `PromiseRegistry::clear()` (called
from `kislay_async_lane_clear()`, itself called from `kislay_app_free_obj()`
on shutdown) just released every remaining promise's refcount without ever
invoking its `then()`/`catch()`/`finally()` callbacks. A registered
`catch()` handler would simply never fire, silently, no warning. Root
cause was narrower than it first looked (previously described as "needs
touching PromiseRegistry semantics, too large to fix confidently") — the
actual fix only needed `kislay_async_lane_clear()` to reject+dispatch each
lane's still-pending promise (via the same `kislay_promise_reject()` /
`kislay_promise_dispatch()` / `unregister_promise()` sequence every normal
resolution path already uses) *before* handing off to `promise_registry->
clear()`'s defensive fallback release. New helper:
`kislay_async_reject_pending_promise()`, applied to both
`lane.pending_php_tasks` and `lane.pending_http_tasks`. The rejection
reason is the string `"Kislay\Core\App stopped before this task
completed"`.

Verified empirically, not just by code reading — `tests/async_promise_shutdown_rejection_test.phpt`
(ZTS-only: needs `listenAsync()`'s non-blocking return to leave a task
genuinely undrained) queues 100 decoy `async()` tasks ahead of a target one
so that `then()`/`catch()`/`finally()`'s own "opportunistic drain" (budget
64 per call — a real gotcha, see below) can't reach the target task before
`unset($app)` destroys the App. Confirmed both directions: fails (callback
silently never fires) against the pre-fix code, passes against the fix.
Full 23/23 ZTS suite green (built via `build_zts_php.sh` +
`third_party`/rsync-scratch pattern — **do not blanket-exclude `config.*`
in that rsync**, it deletes the load-bearing `config.m4` phpize needs,
not just generated build artifacts; exclude `config.h`/`config.status`/
`config.log` by name instead), 20/20 NTS suite green.

**Gotcha for future async/Promise work:** `then()`/`catch()`/`finally()`
each call `kislay_async_drain_lane(app, lane, 64)` once, synchronously,
before checking the promise's own state — an "opportunistic drain" that
processes up to 64 pending tasks for the *entire lane*, not just the
promise being called on. Registering a callback on a promise can resolve
a *different* promise's task as a side effect, and — as this session found
the hard way while writing a test — can resolve the very promise you're
registering the callback on if it's not yet been dispatched.

**Still open, narrower than before:** `PhpRuntimePool::stop()` still
silently drops any `RuntimeRequestMessage` left in `request_queue_`
without calling its `completion->complete(...)` — unrelated to
`PromiseRegistry` entirely (this is the plain-condvar `RequestCompletion`
used for cross-thread request dispatch, not the `Promise` class). Not a
crash, not a leak (bounded, no zvals in `RuntimeRequestMessage`) — a
caller blocked in `RequestCompletion::wait_for(timeout)` just times out at
its own timeout instead of getting an immediate response. Worth a small,
separate, self-contained fix (drain `request_queue_` in `stop()` and
`complete()` each leftover request with a "shutting down" error response)
if this needs closing — doesn't touch `PromiseRegistry` at all.

## Testing

Standard phpt, `make test`. 20/23 (3 ZTS-only tests correctly skipped on
an NTS build; 0 failures) as of 2026-09-13. For libuv-specific work, also
build/test against real ZTS (`./build_zts_php.sh`) — several bugs here
only manifest under ZTS or under Linux, not this machine's default NTS
macOS build; see `Dockerfile.zts-sigbus` for a reusable Linux/arm64 debug
ZTS environment (Docker Desktop on Apple Silicon runs it natively, no
emulation — a genuinely useful "same CPU, different OS" comparison point).

## Known open issues

1. `listenAsync()` + `AsyncHttp`/`Promise` SIGBUS — see above, the
   headline open issue in this entire ecosystem as of 2026-08-30.
2. `PhpRuntimePool::stop()` silently drops queued `RuntimeRequestMessage`s
   without completing them — see "Dropped-promise-on-shutdown fix" above;
   narrower and lower-severity than it looked before 2026-09-13 (the
   `Promise`/`PromiseRegistry` half of that issue is now fixed).
