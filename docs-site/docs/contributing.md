# Contributing to KislayPHP

Thank you for your interest in contributing! This guide covers everything you need to get started.

---

## Building Locally

Each extension is an independent C++ PHP extension. The build flow is the same for all of them:

```bash
# Clone the extension you want to work on
git clone https://github.com/KislayPHP/php-kislay-core
cd php-kislay-core

# Bootstrap
phpize

# Configure (add --enable-debug for debug symbols)
./configure --enable-kislay-core --enable-debug

# Build
make -j$(nproc)

# Install into the active PHP
sudo make install
```

---

## Running Tests

```bash
make test
```

This runs the PHP extension test harness (`run-tests.php`). Test files live in `tests/` and use the `.phpt` format.

To run a specific test:

```bash
php run-tests.php tests/001_basic_routing.phpt
```

---

## C++ Style Guide

- **Standard**: C++17 — use `std::string_view`, `if constexpr`, structured bindings.
- **No exceptions in hot path** — use return-value error codes (`std::expected` or custom `Result<T, E>`).
- **Thread safety** — every shared data structure must be protected by a `std::mutex` or use atomics. Document which mutex guards which state.
- **RAII everywhere** — no raw `new` / `delete`; use `std::unique_ptr`, `std::shared_ptr`, or custom RAII wrappers.
- **Naming**: `snake_case` for variables and functions, `PascalCase` for classes, `UPPER_SNAKE` for constants.
- **Headers**: one class per header where possible; use `#pragma once`.

---

## Pull Request Process

1. **Fork** the repository and create a feature branch: `git checkout -b feat/my-feature`
2. **Write tests** — every bug fix must include a regression test; every feature must include at least one `.phpt` test.
3. **Build and test** — `make -j$(nproc) && make test` must pass with zero failures.
4. **Open a PR** against `main` with a clear description of the change and why.
5. **Review** — a maintainer will review within 5 business days.
6. **Squash merge** — all PRs are squash-merged to keep the history clean.

---

## Issue Labels

| Label | Meaning |
|---|---|
| `bug` | Something is broken |
| `enhancement` | New feature or improvement |
| `windows` | Windows-specific issue |
| `docs` | Documentation only |
| `help wanted` | Good for contributors |
| `good first issue` | Beginner-friendly task |

---

## Code of Conduct

Be respectful and constructive. We follow the [Contributor Covenant](https://www.contributor-covenant.org/version/2/1/code_of_conduct/).
