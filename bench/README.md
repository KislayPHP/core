# Bench Directory

This directory contains benchmark tooling for `kislayphp/core`.

## Policy

- `bench/results/` stores generated benchmark outputs.
- Generated files in `bench/results/` are intentionally not tracked in git.
- Keep only `bench/results/.gitkeep` so the directory exists in fresh clones.

## Typical flow

- Run `./bench/run_benchmark.sh` or `./bench/run_benchmark_stable.sh`.
- Review local outputs in `bench/results/`.
- Publish only curated summary findings in release notes, not raw artifacts.
