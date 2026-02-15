# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260214-211801.md
- Candidate: benchmark-stable-quick-20260214-211809.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 28865.03 | 19593.24 | -32.12% | 50.00% | 57.14% |
| /json | 23145.68 | 4936.03 | -78.67% | 728.57% | 1966.67% |
| /file | 786.19 | 829.09 | 5.46% | -19.46% | -2.82% |

Interpretation: positive Req/sec is better; negative latency delta is better.
