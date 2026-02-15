# KislayPHP Stable Benchmark Delta

- Profile: quick
- Baseline: benchmark-stable-quick-20260214-211526.md
- Candidate: benchmark-stable-quick-20260214-211555.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 27322.70 | 17688.85 | -35.26% | 71.43% | 55.56% |
| /json | 13601.78 | 6427.77 | -52.74% | 40.91% | 1380.00% |
| /file | 2192.25 | 1274.66 | -41.86% | 133.87% | 136.62% |

Interpretation: positive Req/sec is better; negative latency delta is better.
