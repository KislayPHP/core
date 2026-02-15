# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 200
- Requests: 20000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 13627.63 | 23ms | 198ms |
| /json | 24909.83 | 13ms | 41ms |
