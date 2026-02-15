# KislayPHP Stable Benchmark

- Profile: release
- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 15000
- Concurrency: 100
- Warmup requests: 2000
- Repeats: 3

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 10866.27 | 16.00ms | 38.00ms |
| /json | 13004.78 | 13.00ms | 32.00ms |
| /file | 886.35 | 181.00ms | 288.00ms |

Raw outputs:
- raw-stable-release-20260215-010046-_plaintext-run1.txt
- raw-stable-release-20260215-010046-_plaintext-run2.txt
- raw-stable-release-20260215-010046-_plaintext-run3.txt
- raw-stable-release-20260215-010046-_json-run1.txt
- raw-stable-release-20260215-010046-_json-run2.txt
- raw-stable-release-20260215-010046-_json-run3.txt
- raw-stable-release-20260215-010046-_file-run1.txt
- raw-stable-release-20260215-010046-_file-run2.txt
- raw-stable-release-20260215-010046-_file-run3.txt
