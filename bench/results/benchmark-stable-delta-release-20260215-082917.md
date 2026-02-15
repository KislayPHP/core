# KislayPHP Stable Benchmark Delta

- Profile: release
- Baseline: benchmark-stable-release-20260215-082816.md
- Candidate: benchmark-stable-release-20260215-082850.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 20629.50 | 31728.77 | 53.80% | -37.50% | -65.22% |
| /json | 18720.82 | 22180.10 | 18.48% | -42.86% | -50.00% |
| /file | 2028.38 | 2116.86 | 4.36% | -4.92% | -29.52% |

Interpretation: positive Req/sec is better; negative latency delta is better.
