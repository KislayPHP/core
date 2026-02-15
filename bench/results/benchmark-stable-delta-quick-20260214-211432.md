# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260214-211421.md
- Candidate: benchmark-stable-quick-20260214-211424.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 15968.40 | 14339.84 | -10.20% | -13.33% | 16.67% |
| /json | 12738.31 | 14911.40 | 17.06% | -7.69% | 43.75% |
| /file | 1505.05 | 929.08 | -38.27% | 212.50% | 339.16% |

Interpretation: positive Req/sec is better; negative latency delta is better.
