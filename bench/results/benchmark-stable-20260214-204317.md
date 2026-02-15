# KislayPHP Stable Benchmark

- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 15000
- Concurrency: 100
- Warmup requests: 2000
- Repeats: 3

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 18956.94 | 10.00ms | 22.00ms |
| /json | 18183.21 | 11.00ms | 19.00ms |
| /file | 1058.21 | 159.00ms | 429.00ms |

Raw outputs:
- raw-stable-20260214-204317-_plaintext-run1.txt
- raw-stable-20260214-204317-_plaintext-run2.txt
- raw-stable-20260214-204317-_plaintext-run3.txt
- raw-stable-20260214-204317-_json-run1.txt
- raw-stable-20260214-204317-_json-run2.txt
- raw-stable-20260214-204317-_json-run3.txt
- raw-stable-20260214-204317-_file-run1.txt
- raw-stable-20260214-204317-_file-run2.txt
- raw-stable-20260214-204317-_file-run3.txt
