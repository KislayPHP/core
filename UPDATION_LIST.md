# Updation List (Lifecycle APIs)

This list tracks updates for the non-blocking lifecycle work (`listenAsync`, `wait`, `isRunning`, `stop`) and how to validate them.

## 1) Runtime/API updates

- [x] Add non-blocking start API: `listenAsync(host, port, tls?)`
- [x] Add lifecycle wait API: `wait(timeoutMs = -1)`
- [x] Add running-state API: `isRunning()`
- [x] Keep `stop()` idempotent and safe to call multiple times

## 2) Regression coverage updates

- [x] Positive lifecycle flow test: `tests/lifecycle_async_test.phpt`
- [x] Negative timeout validation test: `tests/lifecycle_wait_invalid_test.phpt`
- [x] Negative double-start validation test: `tests/lifecycle_double_listen_test.phpt`

## 3) Automation updates

- [x] Make target: `make test-lifecycle` (runs all lifecycle `.phpt` tests)
- [x] Make target: `make task-updation` (build + lifecycle tests)
- [x] Make target: `make task-dev` (build + lifecycle + quick perf gate)
- [x] Make target: `make task-ci` (build + lifecycle + strict perf gate)
- [x] Make target: `make task-updation-release` (build + lifecycle + release perf gate)

## 4) Documentation updates

- [x] Test commands documented in `tests/README.md`
- [x] Task commands documented in `README.md`

## 5) CI ecosystem updates

- [x] GitHub Actions workflow runs strict ecosystem task (`make task-ci`)

## Execute full updation task

```bash
cd https
make task-updation
```

## Execute complete development ecosystem tasks

```bash
cd https
make task-dev
make task-ci
make task-updation-release
```
