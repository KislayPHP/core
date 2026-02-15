# KislayPHP Stable Benchmark

- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 5000
- Concurrency: 100
- Warmup requests: 500
- Repeats: 1

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 21279.77 | 9.00ms | 13.00ms |
| /json | 16572.92 | 12.00ms | 24.00ms |
| /file | 800.71 | 448.00ms | 1026.00ms |

Raw outputs:
- raw-stable-20260214-210259-_plaintext-run1.txt
- raw-stable-20260214-210259-_json-run1.txt
- raw-stable-20260214-210259-_file-run1.txt
