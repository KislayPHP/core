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
| /plaintext | 22406.62 | 8.00ms | 18.00ms |
| /json | 26625.91 | 6.00ms | 9.00ms |
| /file | 740.75 | 394.00ms | 509.00ms |

Raw outputs:
- raw-stable-release-20260215-081757-_plaintext-run1.txt
- raw-stable-release-20260215-081757-_plaintext-run2.txt
- raw-stable-release-20260215-081757-_plaintext-run3.txt
- raw-stable-release-20260215-081757-_json-run1.txt
- raw-stable-release-20260215-081757-_json-run2.txt
- raw-stable-release-20260215-081757-_json-run3.txt
- raw-stable-release-20260215-081757-_file-run1.txt
- raw-stable-release-20260215-081757-_file-run2.txt
- raw-stable-release-20260215-081757-_file-run3.txt
