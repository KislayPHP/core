#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
RESULTS_DIR="$BENCH_DIR/results"
mkdir -p "$RESULTS_DIR"

HOST="${BENCH_HOST:-127.0.0.1}"
PORT="${BENCH_PORT:-9090}"
TARGET_PATH="${BENCH_TARGET_PATH:-/plaintext}"
DURATION="${BENCH_DURATION:-20}"
CONCURRENCY="${BENCH_CONCURRENCY:-200}"
THREADS="${BENCH_THREADS:-4}"
REQUESTS="${BENCH_REQUESTS:-50000}"

EXT_PATH="${KISLAY_EXTENSION_PATH:-$ROOT_DIR/modules/kislayphp_extension.so}"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
OUT_FILE="$RESULTS_DIR/bench-$(date +%Y%m%d-%H%M%S).txt"

if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found at: $EXT_PATH"
  echo "Build first: cd $ROOT_DIR && make"
  exit 1
fi

echo "Starting benchmark server..."
php -c /dev/null -d extension="$EXT_PATH" "$SERVER_SCRIPT" >"$RESULTS_DIR/server.log" 2>&1 &
SERVER_PID=$!

cleanup() {
  if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for _ in {1..40}; do
  if curl -sf "http://$HOST:$PORT/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

if ! curl -sf "http://$HOST:$PORT/health" >/dev/null 2>&1; then
  echo "Server failed to start. Check: $RESULTS_DIR/server.log"
  exit 1
fi

URL="http://$HOST:$PORT$TARGET_PATH"

echo "Benchmark target: $URL" | tee "$OUT_FILE"
echo "Duration: ${DURATION}s, Concurrency: $CONCURRENCY" | tee -a "$OUT_FILE"

echo "" | tee -a "$OUT_FILE"
if command -v wrk >/dev/null 2>&1; then
  echo "Tool: wrk" | tee -a "$OUT_FILE"
  wrk -t"$THREADS" -c"$CONCURRENCY" -d"${DURATION}s" --latency "$URL" | tee -a "$OUT_FILE"
elif command -v ab >/dev/null 2>&1; then
  echo "Tool: ab" | tee -a "$OUT_FILE"
  ab -n "$REQUESTS" -c "$CONCURRENCY" "$URL" | tee -a "$OUT_FILE"
else
  echo "No benchmark tool found. Install one:" | tee -a "$OUT_FILE"
  echo "  brew install wrk" | tee -a "$OUT_FILE"
  echo "  or use ApacheBench (ab)" | tee -a "$OUT_FILE"
  exit 1
fi

echo "" | tee -a "$OUT_FILE"
echo "Saved report: $OUT_FILE" | tee -a "$OUT_FILE"
