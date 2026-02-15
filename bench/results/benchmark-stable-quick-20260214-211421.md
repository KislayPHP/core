# KislayPHP Stable Benchmark

- Profile: quick
- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 3000
- Concurrency: 100
- Warmup requests: 300
- Repeats: 1

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 15968.40 | 15.00ms | 18.00ms |
| /json | 12738.31 | 13.00ms | 16.00ms |
| /file | 1505.05 | 120.00ms | 143.00ms |

Raw outputs:
- raw-stable-quick-20260214-211421-_plaintext-run1.txt
- raw-stable-quick-20260214-211421-_json-run1.txt
- raw-stable-quick-20260214-211421-_file-run1.txt
