# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 8506.05 | 28ms | 47ms |
| /json | 20560.51 | 9ms | 12ms |
| /file | 1303.28 | 134ms | 479ms |

Raw outputs:
- raw-20260214-202627-_plaintext.txt
- raw-20260214-202627-_json.txt
- raw-20260214-202627-_file.txt
