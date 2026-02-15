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
| /plaintext | 22199.14 | 8.00ms | 9.00ms |
| /json | 16988.60 | 14.00ms | 43.00ms |
| /file | 1264.27 | 151.00ms | 282.00ms |

Raw outputs:
- raw-stable-quick-20260214-211653-_plaintext-run1.txt
- raw-stable-quick-20260214-211653-_json-run1.txt
- raw-stable-quick-20260214-211653-_file-run1.txt
