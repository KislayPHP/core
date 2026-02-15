# KislayPHP Stable Benchmark Delta

- Profile: release
- Baseline: benchmark-stable-release-20260215-082610.md
- Candidate: benchmark-stable-release-20260215-082703.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 14823.56 | 16066.22 | 8.38% | -15.38% | 14.29% |
| /json | 18158.49 | 17426.28 | -4.03% | 0.00% | -11.76% |
| /file | 1168.12 | 1417.25 | 21.33% | -22.15% | -9.00% |

Interpretation: positive Req/sec is better; negative latency delta is better.
