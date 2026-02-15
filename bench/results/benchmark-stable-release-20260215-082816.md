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
| /plaintext | 20629.50 | 8.00ms | 23.00ms |
| /json | 18720.82 | 7.00ms | 12.00ms |
| /file | 2028.38 | 61.00ms | 105.00ms |

Raw outputs:
- raw-stable-release-20260215-082816-_plaintext-run1.txt
- raw-stable-release-20260215-082816-_plaintext-run2.txt
- raw-stable-release-20260215-082816-_plaintext-run3.txt
- raw-stable-release-20260215-082816-_json-run1.txt
- raw-stable-release-20260215-082816-_json-run2.txt
- raw-stable-release-20260215-082816-_json-run3.txt
- raw-stable-release-20260215-082816-_file-run1.txt
- raw-stable-release-20260215-082816-_file-run2.txt
- raw-stable-release-20260215-082816-_file-run3.txt
