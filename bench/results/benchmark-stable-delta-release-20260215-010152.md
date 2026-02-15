# KislayPHP Stable Benchmark Delta

- Profile: release
- Baseline: benchmark-stable-release-20260215-005944.md
- Candidate: benchmark-stable-release-20260215-010046.md

| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |
|---|---:|---:|---:|---:|---:|
| /plaintext | 13147.11 | 10866.27 | -17.35% | 6.67% | 80.95% |
| /json | 10697.41 | 13004.78 | 21.57% | -35.00% | -11.11% |
| /file | 852.74 | 886.35 | 3.94% | -18.47% | -23.81% |

Interpretation: positive Req/sec is better; negative latency delta is better.
