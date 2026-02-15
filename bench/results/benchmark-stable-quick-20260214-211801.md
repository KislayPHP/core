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
| /plaintext | 28865.03 | 6.00ms | 7.00ms |
| /json | 23145.68 | 7.00ms | 9.00ms |
| /file | 786.19 | 334.00ms | 425.00ms |

Raw outputs:
- raw-stable-quick-20260214-211801-_plaintext-run1.txt
- raw-stable-quick-20260214-211801-_json-run1.txt
- raw-stable-quick-20260214-211801-_file-run1.txt
