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
| /plaintext | 16066.22 | 11.00ms | 24.00ms |
| /json | 17426.28 | 10.00ms | 15.00ms |
| /file | 1417.25 | 116.00ms | 263.00ms |

Raw outputs:
- raw-stable-release-20260215-082703-_plaintext-run1.txt
- raw-stable-release-20260215-082703-_plaintext-run2.txt
- raw-stable-release-20260215-082703-_plaintext-run3.txt
- raw-stable-release-20260215-082703-_json-run1.txt
- raw-stable-release-20260215-082703-_json-run2.txt
- raw-stable-release-20260215-082703-_json-run3.txt
- raw-stable-release-20260215-082703-_file-run1.txt
- raw-stable-release-20260215-082703-_file-run2.txt
- raw-stable-release-20260215-082703-_file-run3.txt
