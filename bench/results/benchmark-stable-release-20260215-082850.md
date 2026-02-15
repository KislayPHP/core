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
| /plaintext | 31728.77 | 5.00ms | 8.00ms |
| /json | 22180.10 | 4.00ms | 6.00ms |
| /file | 2116.86 | 58.00ms | 74.00ms |

Raw outputs:
- raw-stable-release-20260215-082850-_plaintext-run1.txt
- raw-stable-release-20260215-082850-_plaintext-run2.txt
- raw-stable-release-20260215-082850-_plaintext-run3.txt
- raw-stable-release-20260215-082850-_json-run1.txt
- raw-stable-release-20260215-082850-_json-run2.txt
- raw-stable-release-20260215-082850-_json-run3.txt
- raw-stable-release-20260215-082850-_file-run1.txt
- raw-stable-release-20260215-082850-_file-run2.txt
- raw-stable-release-20260215-082850-_file-run3.txt
