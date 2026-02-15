# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260214-211424.md
- Candidate: benchmark-stable-quick-20260214-211526.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 14339.84 | 27322.70 | 90.54% | -46.15% | -57.14% |
| /json | 14911.40 | 13601.78 | -8.78% | 83.33% | 8.70% |
| /file | 929.08 | 2192.25 | 135.96% | -83.47% | -88.69% |

Interpretation: positive Req/sec is better; negative latency delta is better.
