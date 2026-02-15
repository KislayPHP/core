# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 20
- Requests: 1000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 10607.04 | 5ms | 6ms |
| /json | 19343.48 | 2ms | 3ms |
| /file | 2293.86 | 16ms | 25ms |

Raw outputs:
- raw-20260214-200538-_plaintext.txt
- raw-20260214-200538-_json.txt
- raw-20260214-200538-_file.txt
