# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260214-211555.md
- Candidate: benchmark-stable-quick-20260214-211653.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 17688.85 | 22199.14 | 25.50% | -33.33% | -35.71% |
| /json | 6427.77 | 16988.60 | 164.30% | -54.84% | -88.38% |
| /file | 1274.66 | 1264.27 | -0.82% | 4.14% | 67.86% |

Interpretation: positive Req/sec is better; negative latency delta is better.
