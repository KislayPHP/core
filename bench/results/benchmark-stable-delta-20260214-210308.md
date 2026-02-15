# KislayPHP Stable Benchmark Delta

- Baseline: benchmark-stable-20260214-204834.md
- Candidate: benchmark-stable-20260214-210259.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 20814.34 | 21279.77 | 2.24% | 12.50% | -27.78% |
| /json | 18319.09 | 16572.92 | -9.53% | 33.33% | 41.18% |
| /file | 1264.75 | 800.71 | -36.69% | 196.69% | 300.78% |

Interpretation: positive Req/sec is better; negative latency delta is better.
