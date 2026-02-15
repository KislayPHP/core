# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 200
- Requests: 20000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 22424.57 | 16ms | 72ms |
| /json | 19651.89 | 15ms | 108ms |
| /file | 3130.52 | 85ms | 157ms |

Raw outputs:
- raw-20260214-201209-_plaintext.txt
- raw-20260214-201209-_json.txt
- raw-20260214-201209-_file.txt
