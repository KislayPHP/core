# KislayPHP Stable Benchmark

- Profile: quick
- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 5000
- Concurrency: 100
- Warmup requests: 500
- Repeats: 1

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 33005.91 | 6.00ms | 8.00ms |
| /json | 29169.15 | 6.00ms | 26.00ms |
| /file | 1785.08 | 88.00ms | 152.00ms |

Raw outputs:
- raw-stable-quick-20260215-003824-_plaintext-run1.txt
- raw-stable-quick-20260215-003824-_json-run1.txt
- raw-stable-quick-20260215-003824-_file-run1.txt
