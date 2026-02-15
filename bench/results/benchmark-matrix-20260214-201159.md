# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 200
- Requests: 20000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 15317.45 | 26ms | 50ms |
| /json | 12838.09 | 26ms | 288ms |
| /file | 2875.49 | 99ms | 144ms |

Raw outputs:
- raw-20260214-201159-_plaintext.txt
- raw-20260214-201159-_json.txt
- raw-20260214-201159-_file.txt
