# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 12201.55 | 15ms | 38ms |
| /json | 11091.80 | 18ms | 33ms |
| /file | 1013.76 | 202ms | 251ms |

Raw outputs:
- raw-20260214-202111-_plaintext.txt
- raw-20260214-202111-_json.txt
- raw-20260214-202111-_file.txt
