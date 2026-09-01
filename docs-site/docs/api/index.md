# C++ API Reference (Doxygen)

Each extension repository ships a `Doxyfile` in its root for generating C++ internals documentation. **These are not currently published anywhere** — there is no hosted Doxygen site for any module yet (despite this page previously claiming one at `kislayphp.github.io/<module>/api/`; those URLs 404). Generate locally with the instructions below if you need this.

---

## Generating Locally

Each repository ships a `Doxyfile` in its root. To regenerate:

```bash
cd core
doxygen Doxyfile
# Output written to docs/html/
```

Requirements: Doxygen 1.9+ and Graphviz (for call graphs).

---

## PHP Extension API

The PHP-visible API (classes, methods, constants) is documented inline in each extension's `README.md` and on the relevant page of this documentation site.

| Extension | README |
|---|---|
| Core | [core README](https://github.com/KislayPHP/core#readme) |
| Gateway | [gateway README](https://github.com/KislayPHP/gateway#readme) |
| Discovery | [discovery README](https://github.com/KislayPHP/discovery#readme) |
| EventBus | [eventbus README](https://github.com/KislayPHP/eventbus#readme) |
| Queue | [queue README](https://github.com/KislayPHP/queue#readme) |
| Metrics | [metrics README](https://github.com/KislayPHP/metrics#readme) |
| Persistence | [persistence README](https://github.com/KislayPHP/persistence#readme) |
| Config | [config README](https://github.com/KislayPHP/config#readme) |
