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
| /plaintext | 19593.24 | 9.00ms | 11.00ms |
| /json | 4936.03 | 58.00ms | 186.00ms |
| /file | 829.09 | 269.00ms | 413.00ms |

Raw outputs:
- raw-stable-quick-20260214-211809-_plaintext-run1.txt
- raw-stable-quick-20260214-211809-_json-run1.txt
- raw-stable-quick-20260214-211809-_file-run1.txt
