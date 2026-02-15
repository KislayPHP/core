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
| /plaintext | 14823.56 | 13.00ms | 21.00ms |
| /json | 18158.49 | 10.00ms | 17.00ms |
| /file | 1168.12 | 149.00ms | 289.00ms |

Raw outputs:
- raw-stable-release-20260215-082610-_plaintext-run1.txt
- raw-stable-release-20260215-082610-_plaintext-run2.txt
- raw-stable-release-20260215-082610-_plaintext-run3.txt
- raw-stable-release-20260215-082610-_json-run1.txt
- raw-stable-release-20260215-082610-_json-run2.txt
- raw-stable-release-20260215-082610-_json-run3.txt
- raw-stable-release-20260215-082610-_file-run1.txt
- raw-stable-release-20260215-082610-_file-run2.txt
- raw-stable-release-20260215-082610-_file-run3.txt
