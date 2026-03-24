#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
EXT_PATH="$ROOT_DIR/modules/kislayphp_extension.so"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
CLIENT_SCRIPT="$BENCH_DIR/simple_bench_client.php"

echo "=== High Concurrency Performance Test ==="

# Start server with 4 threads
export KISLAYPHP_HTTP_THREADS=4
php -n -d extension="$EXT_PATH" "$SERVER_SCRIPT" > /dev/null 2>&1 &
SERVER_PID=$!

cleanup() {
  echo "Cleaning up..."
  kill "$SERVER_PID" || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "Waiting for server..."
sleep 2

URL="http://127.0.0.1:9090/plaintext"
CLIENTS=4
C_PER_CLIENT=50
N_PER_CLIENT=2000

echo "Spawning $CLIENTS clients, each with $C_PER_CLIENT concurrency and $N_PER_CLIENT requests..."
START_TIME=$(date +%s.%N)

for i in $(seq 1 $CLIENTS); do
  php "$CLIENT_SCRIPT" "$URL" "$C_PER_CLIENT" "$N_PER_CLIENT" > "bench/client_$i.log" &
done

wait

END_TIME=$(date +%s.%N)
DURATION=$(echo "$END_TIME - $START_TIME" | bc)
TOTAL_REQUESTS=$(($CLIENTS * $N_PER_CLIENT))
RPS=$(echo "scale=2; $TOTAL_REQUESTS / $DURATION" | bc)

echo "------------------------------------------------"
echo "Overall Results:"
echo "Total Time: $DURATION seconds"
echo "Total Requests: $TOTAL_REQUESTS"
echo "Combined Requests Per Second: $RPS"
echo "------------------------------------------------"

for i in $(seq 1 $CLIENTS); do
  echo "Client $i details:"
  tail -n 5 "bench/client_$i.log"
  rm "bench/client_$i.log"
done

echo "=== Test Completed ==="
