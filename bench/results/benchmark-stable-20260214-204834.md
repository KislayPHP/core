# KislayPHP Stable Benchmark

- Tool: ab
- Host: 127.0.0.1:9090
- Requests per run: 15000
- Concurrency: 100
- Warmup requests: 2000
- Repeats: 3

| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |
|---|---:|---:|---:|
| /plaintext | 20814.34 | 8.00ms | 18.00ms |
| /json | 18319.09 | 9.00ms | 17.00ms |
| /file | 1264.75 | 151.00ms | 256.00ms |

Raw outputs:
- raw-stable-20260214-204834-_plaintext-run1.txt
- raw-stable-20260214-204834-_plaintext-run2.txt
- raw-stable-20260214-204834-_plaintext-run3.txt
- raw-stable-20260214-204834-_json-run1.txt
- raw-stable-20260214-204834-_json-run2.txt
- raw-stable-20260214-204834-_json-run3.txt
- raw-stable-20260214-204834-_file-run1.txt
- raw-stable-20260214-204834-_file-run2.txt
- raw-stable-20260214-204834-_file-run3.txt
