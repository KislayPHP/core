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
| /plaintext | 17688.85 | 12.00ms | 14.00ms |
| /json | 6427.77 | 31.00ms | 370.00ms |
| /file | 1274.66 | 145.00ms | 168.00ms |

Raw outputs:
- raw-stable-quick-20260214-211555-_plaintext-run1.txt
- raw-stable-quick-20260214-211555-_json-run1.txt
- raw-stable-quick-20260214-211555-_file-run1.txt
