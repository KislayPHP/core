# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 6239.38 | 25ms | 251ms |
| /json | 7477.73 | 29ms | 48ms |
| /file | 499.59 | 446ms | 1153ms |

Raw outputs:
- raw-20260214-201903-_plaintext.txt
- raw-20260214-201903-_json.txt
- raw-20260214-201903-_file.txt
