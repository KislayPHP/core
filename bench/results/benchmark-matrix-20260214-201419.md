# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 11947.36 | 14ms | 21ms |
| /json | 29183.96 | 6ms | 8ms |
| /file | 1598.19 | 118ms | 224ms |

Raw outputs:
- raw-20260214-201419-_plaintext.txt
- raw-20260214-201419-_json.txt
- raw-20260214-201419-_file.txt
