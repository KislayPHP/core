# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260215-003819.md
- Candidate: benchmark-stable-quick-20260215-003824.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 27976.88 | 33005.91 | 17.98% | -14.29% | -20.00% |
| /json | 29944.60 | 29169.15 | -2.59% | 0.00% | 225.00% |
| /file | 1572.69 | 1785.08 | 13.50% | -40.14% | -17.84% |

Interpretation: positive Req/sec is better; negative latency delta is better.
