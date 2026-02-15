# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 14597.16 | 17ms | 45ms |
| /json | 21322.90 | 8ms | 22ms |
| /file | 1598.93 | 102ms | 157ms |

Raw outputs:
- raw-20260214-202642-_plaintext.txt
- raw-20260214-202642-_json.txt
- raw-20260214-202642-_file.txt
