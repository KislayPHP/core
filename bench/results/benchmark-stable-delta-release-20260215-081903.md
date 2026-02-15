# KislayPHP Stable Benchmark Delta

- Profile: release
- Baseline: benchmark-stable-release-20260215-081703.md
- Candidate: benchmark-stable-release-20260215-081757.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 18442.83 | 22406.62 | 21.49% | 0.00% | 63.64% |
| /json | 18665.52 | 26625.91 | 42.65% | -33.33% | -75.68% |
| /file | 1062.96 | 740.75 | -30.31% | 74.34% | 47.11% |

Interpretation: positive Req/sec is better; negative latency delta is better.
