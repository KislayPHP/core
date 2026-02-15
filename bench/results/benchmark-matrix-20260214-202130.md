# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 18809.37 | 10ms | 16ms |
| /json | 18586.32 | 8ms | 10ms |
| /file | 926.87 | 226ms | 874ms |

Raw outputs:
- raw-20260214-202130-_plaintext.txt
- raw-20260214-202130-_json.txt
- raw-20260214-202130-_file.txt
