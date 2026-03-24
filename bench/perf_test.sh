#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
EXT_PATH="$ROOT_DIR/modules/kislayphp_extension.so"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
CLIENT_SCRIPT="$BENCH_DIR/simple_bench_client.php"
PORT=9091

if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found. Building..."
  make
fi

echo "=== Starting Performance Test ==="

# Start server
php -n -d extension="$EXT_PATH" "$SERVER_SCRIPT" > /dev/null 2>&1 &
SERVER_PID=$!
export BENCH_PORT=$PORT

cleanup() {
  echo "Cleaning up..."
  kill "$SERVER_PID" || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for server
echo "Waiting for server to start on port $PORT..."
for _ in {1..20}; do
  if curl -sf "http://127.0.0.1:9090/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

echo "Server is ready."
echo "------------------------------------------------"

run_bench() {
    local path=$1
    local c=$2
    local n=$3
    echo "Endpoint: $path | Concurrency: $c | Requests: $n"
    php "$CLIENT_SCRIPT" "http://127.0.0.1:9090$path" "$c" "$n"
    echo "------------------------------------------------"
}

# Run benchmarks
run_bench "/plaintext" 50 5000
run_bench "/json" 50 5000
run_bench "/plaintext" 200 10000
run_bench "/json" 200 10000

echo "=== Performance Test Completed ==="
