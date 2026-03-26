#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
EXT_PATH="${KISLAY_EXTENSION_PATH:-$ROOT_DIR/modules/kislayphp_extension.so}"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
PORT=9091
HOST="${BENCH_HOST:-127.0.0.1}"
REQUESTS_MEDIUM="${BENCH_REQUESTS_MEDIUM:-5000}"
REQUESTS_HIGH="${BENCH_REQUESTS_HIGH:-20000}"
CONCURRENCY_MEDIUM="${BENCH_CONCURRENCY_MEDIUM:-50}"
CONCURRENCY_HIGH="${BENCH_CONCURRENCY_HIGH:-100}"
BENCH_WORKERS="${BENCH_WORKERS:-10}"
BENCH_HTTP_THREADS="${BENCH_HTTP_THREADS:-8}"

if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found. Building..."
  make
fi

if command -v wrk >/dev/null 2>&1; then
  TOOL="wrk"
elif command -v ab >/dev/null 2>&1; then
  TOOL="ab"
else
  echo "No benchmark tool found. Install wrk or ApacheBench (ab)."
  exit 1
fi

echo "=== Starting Performance Test ==="

# Start server
env \
  BENCH_PORT="$PORT" \
  BENCH_HOST="$HOST" \
  BENCH_WORKERS="$BENCH_WORKERS" \
  BENCH_HTTP_THREADS="$BENCH_HTTP_THREADS" \
  KISLAYPHP_HTTP_LOG=0 \
  KISLAYPHP_HTTP_REQUEST_ID=0 \
  KISLAYPHP_HTTP_TRACE=0 \
  php -n -d extension="$EXT_PATH" "$SERVER_SCRIPT" > /dev/null 2>&1 &
SERVER_PID=$!

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
    if [[ "$TOOL" == "wrk" ]]; then
        local duration=${BENCH_DURATION:-10}
        wrk -t"${BENCH_THREADS:-4}" -c"$c" -d"${duration}s" --latency "http://$HOST:$PORT$path"
    else
        ab -n "$n" -c "$c" -s 60 "http://$HOST:$PORT$path"
    fi
    echo "------------------------------------------------"
}

# Run benchmarks
run_bench "/plaintext" "$CONCURRENCY_MEDIUM" "$REQUESTS_MEDIUM"
run_bench "/json" "$CONCURRENCY_MEDIUM" "$REQUESTS_MEDIUM"
run_bench "/plaintext" "$CONCURRENCY_HIGH" "$REQUESTS_HIGH"
run_bench "/json" "$CONCURRENCY_HIGH" "$REQUESTS_HIGH"

echo "=== Performance Test Completed ==="
