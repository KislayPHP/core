#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
RESULTS_DIR="$BENCH_DIR/results"
mkdir -p "$RESULTS_DIR"

HOST="${BENCH_HOST:-127.0.0.1}"
PORT="${BENCH_PORT:-9090}"
DURATION="${BENCH_DURATION:-10}"
CONCURRENCY="${BENCH_CONCURRENCY:-100}"
THREADS="${BENCH_THREADS:-4}"
REQUESTS="${BENCH_REQUESTS:-20000}"

EXT_PATH="${KISLAY_EXTENSION_PATH:-$ROOT_DIR/modules/kislayphp_extension.so}"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
SUMMARY_FILE="$RESULTS_DIR/benchmark-matrix-$RUN_ID.md"

if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found at: $EXT_PATH"
  echo "Build first: cd $ROOT_DIR && make"
  exit 1
fi

if command -v wrk >/dev/null 2>&1; then
  TOOL="wrk"
elif command -v ab >/dev/null 2>&1; then
  TOOL="ab"
else
  echo "No benchmark tool found. Install one:"
  echo "  brew install wrk"
  echo "  or install ApacheBench (ab)"
  exit 1
fi

php -c /dev/null -d extension="$EXT_PATH" "$SERVER_SCRIPT" >"$RESULTS_DIR/server-$RUN_ID.log" 2>&1 &
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
  echo "Server failed to start. Check: $RESULTS_DIR/server-$RUN_ID.log"
  exit 1
fi

ENDPOINTS=("/plaintext" "/json" "/file")

echo "# KislayPHP Benchmark Matrix" > "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "- Tool: $TOOL" >> "$SUMMARY_FILE"
echo "- Host: $HOST:$PORT" >> "$SUMMARY_FILE"
echo "- Concurrency: $CONCURRENCY" >> "$SUMMARY_FILE"
if [[ "$TOOL" == "wrk" ]]; then
  echo "- Duration: ${DURATION}s" >> "$SUMMARY_FILE"
  echo "- Threads: $THREADS" >> "$SUMMARY_FILE"
else
  echo "- Requests: $REQUESTS" >> "$SUMMARY_FILE"
fi

echo "" >> "$SUMMARY_FILE"
echo "| Endpoint | Req/sec | P95 | P99 |" >> "$SUMMARY_FILE"
echo "|---|---:|---:|---:|" >> "$SUMMARY_FILE"

trim() { sed 's/^[[:space:]]*//;s/[[:space:]]*$//'; }

for path in "${ENDPOINTS[@]}"; do
  url="http://$HOST:$PORT$path"
  raw="$RESULTS_DIR/raw-${RUN_ID}-$(echo "$path" | tr '/' '_').txt"

  if [[ "$TOOL" == "wrk" ]]; then
    wrk -t"$THREADS" -c"$CONCURRENCY" -d"${DURATION}s" --latency "$url" > "$raw"

    req_sec=$(grep -E "Requests/sec:" "$raw" | awk '{print $2}' | trim)
    p95=$(grep -E "^[[:space:]]*95%" "$raw" | awk '{print $2}' | trim)
    p99=$(grep -E "^[[:space:]]*99%" "$raw" | awk '{print $2}' | trim)
  else
    ab -n "$REQUESTS" -c "$CONCURRENCY" "$url" > "$raw"

    req_sec=$(grep -E "Requests per second:" "$raw" | awk '{print $4}' | trim)
    p95=$(grep -E "^[[:space:]]*95%" "$raw" | awk '{print $2"ms"}' | trim)
    p99=$(grep -E "^[[:space:]]*99%" "$raw" | awk '{print $2"ms"}' | trim)
  fi

  req_sec=${req_sec:-N/A}
  p95=${p95:-N/A}
  p99=${p99:-N/A}

  echo "| $path | $req_sec | $p95 | $p99 |" >> "$SUMMARY_FILE"
done

echo "" >> "$SUMMARY_FILE"
echo "Raw outputs:" >> "$SUMMARY_FILE"
for path in "${ENDPOINTS[@]}"; do
  echo "- raw-${RUN_ID}-$(echo "$path" | tr '/' '_').txt" >> "$SUMMARY_FILE"
done

echo "Saved summary: $SUMMARY_FILE"
