# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 20
- Requests: 1000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 12515.33 | 3ms | 5ms |
| /json | 1491.48 | 27ms | 36ms |
| /file | 215.86 | 260ms | 631ms |

Raw outputs:
- raw-20260214-200910-_plaintext.txt
- raw-20260214-200910-_json.txt
- raw-20260214-200910-_file.txt
