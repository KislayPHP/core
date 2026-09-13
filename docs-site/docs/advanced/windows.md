# Windows Support

**Windows is not a supported platform yet.** There is an experimental,
best-effort CI build for `core` (`continue-on-error`, tracked as a
"Phase 3" goal in `.github/workflows/ci.yml`), but no released binaries,
no confirmed-working manual build path, and no Windows-specific code in
the runtime itself. Treat everything below as the current experimental
state, not a supported install path.

---

## What's actually missing

- **No `config.w32`.** PHP extensions build on Windows via a `config.w32`
  script (the Windows analogue of `config.m4`/`configure`) — this repo
  doesn't have one, in `core` or anywhere else in the KislayPHP ecosystem.
  Without it, the standard PHP SDK build flow has nothing to drive.
- **No Windows fallback for fork-based workers.** `App`'s multi-worker
  mode (`worker_count > 1`) calls POSIX `fork()`/`waitpid()`/`kill()`
  directly and unconditionally — there is no alternate thread-based
  implementation for Windows. If you need more than one worker process
  today, Windows isn't an option; run a single worker, or use a process
  supervisor to run multiple independent instances on different ports
  instead.
- **libuv and civetweb third-party builds aren't verified on Windows.**
  Both are vendored/linked via Unix-style build tooling (`pkg-config` for
  libuv, autoconf-style detection for civetweb/OpenSSL/curl in
  `config.m4`) that isn't exercised on Windows at all.

## What the experimental CI job does

`.github/workflows/ci.yml` has a `windows` job, marked
`continue-on-error: true`, that:

1. Clones the [PHP SDK Binary Tools](https://github.com/php/php-sdk-binary-tools).
2. Runs `phpize` / `configure --enable-kislayphp_extension` / `nmake`
   inside the SDK's Developer Command Prompt environment.

It is not gating — a red Windows job doesn't fail CI — and its own
comment in the workflow file says full support is tracked as future work.
If you want to experiment, that job is the current best reference for the
exact commands attempted, not a guarantee they produce a working build.

## If you want to help

Real Windows support needs, roughly in order:
1. A `config.w32` for `core` (and each other module you need) mirroring
   `config.m4`'s dependency checks (civetweb, curl, OpenSSL, libuv).
2. A non-`fork()` implementation of multi-worker mode, or an explicit,
   documented single-worker-only restriction on Windows.
3. Verified builds of the vendored third-party libraries (civetweb,
   libuv) against MSVC.

Until then, use Linux or macOS.
