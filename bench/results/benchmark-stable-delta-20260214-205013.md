# KislayPHP Stable Benchmark Delta

- Baseline: benchmark-stable-20260214-204317.md
- Candidate: benchmark-stable-20260214-204834.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 18956.94 | 20814.34 | 9.80% | -20.00% | -18.18% |
| /json | 18183.21 | 18319.09 | 0.75% | -18.18% | -10.53% |
| /file | 1058.21 | 1264.75 | 19.52% | -5.03% | -40.33% |

Interpretation: positive Req/sec is better; negative latency delta is better.
