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
| /plaintext | 18442.83 | 8.00ms | 11.00ms |
| /json | 18665.52 | 9.00ms | 37.00ms |
| /file | 1062.96 | 226.00ms | 346.00ms |

Raw outputs:
- raw-stable-release-20260215-081703-_plaintext-run1.txt
- raw-stable-release-20260215-081703-_plaintext-run2.txt
- raw-stable-release-20260215-081703-_plaintext-run3.txt
- raw-stable-release-20260215-081703-_json-run1.txt
- raw-stable-release-20260215-081703-_json-run2.txt
- raw-stable-release-20260215-081703-_json-run3.txt
- raw-stable-release-20260215-081703-_file-run1.txt
- raw-stable-release-20260215-081703-_file-run2.txt
- raw-stable-release-20260215-081703-_file-run3.txt
