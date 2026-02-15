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
| /plaintext | 27976.88 | 7.00ms | 10.00ms |
| /json | 29944.60 | 6.00ms | 8.00ms |
| /file | 1572.69 | 147.00ms | 185.00ms |

Raw outputs:
- raw-stable-quick-20260215-003819-_plaintext-run1.txt
- raw-stable-quick-20260215-003819-_json-run1.txt
- raw-stable-quick-20260215-003819-_file-run1.txt
