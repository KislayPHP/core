# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 6500.75 | 34ms | 57ms |
| /json | 7350.64 | 33ms | 61ms |
| /file | 499.76 | 370ms | 869ms |

Raw outputs:
- raw-20260214-201938-_plaintext.txt
- raw-20260214-201938-_json.txt
- raw-20260214-201938-_file.txt
