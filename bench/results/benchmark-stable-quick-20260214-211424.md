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
| /plaintext | 14339.84 | 13.00ms | 21.00ms |
| /json | 14911.40 | 12.00ms | 23.00ms |
| /file | 929.08 | 375.00ms | 628.00ms |

Raw outputs:
- raw-stable-quick-20260214-211424-_plaintext-run1.txt
- raw-stable-quick-20260214-211424-_json-run1.txt
- raw-stable-quick-20260214-211424-_file-run1.txt
