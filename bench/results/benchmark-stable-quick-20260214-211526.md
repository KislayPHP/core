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
| /plaintext | 27322.70 | 7.00ms | 9.00ms |
| /json | 13601.78 | 22.00ms | 25.00ms |
| /file | 2192.25 | 62.00ms | 71.00ms |

Raw outputs:
- raw-stable-quick-20260214-211526-_plaintext-run1.txt
- raw-stable-quick-20260214-211526-_json-run1.txt
- raw-stable-quick-20260214-211526-_file-run1.txt
