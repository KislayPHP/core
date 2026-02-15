# KislayPHP Benchmark Matrix

- Tool: ab
- Host: 127.0.0.1:9090
- Concurrency: 100
- Requests: 15000

| Endpoint | Req/sec | P95 | P99 |
|---|---:|---:|---:|
| /plaintext | 29853.78 | 7ms | 9ms |
| /json | 30917.13 | 4ms | 10ms |
| /file | 1929.18 | 77ms | 103ms |

Raw outputs:
- raw-20260214-201431-_plaintext.txt
- raw-20260214-201431-_json.txt
- raw-20260214-201431-_file.txt
