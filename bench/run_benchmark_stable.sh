#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT_DIR/bench"
RESULTS_DIR="$BENCH_DIR/results"
mkdir -p "$RESULTS_DIR"

PROFILE="${BENCH_PROFILE:-default}"

HOST="${BENCH_HOST:-127.0.0.1}"
PORT="${BENCH_PORT:-9090}"
DURATION="${BENCH_DURATION:-20}"
CONCURRENCY="${BENCH_CONCURRENCY:-100}"
THREADS="${BENCH_THREADS:-4}"
REQUESTS="${BENCH_REQUESTS:-15000}"
REPEATS="${BENCH_REPEATS:-3}"
WARMUP_SECONDS="${BENCH_WARMUP_SECONDS:-5}"
WARMUP_REQUESTS="${BENCH_WARMUP_REQUESTS:-2000}"
ENDPOINTS_RAW="${BENCH_ENDPOINTS:-/plaintext /json /file}"

EXT_PATH="${KISLAY_EXTENSION_PATH:-$ROOT_DIR/modules/kislayphp_extension.so}"
SERVER_SCRIPT="$BENCH_DIR/benchmark_server.php"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
SUMMARY_FILE="$RESULTS_DIR/benchmark-stable-${PROFILE}-$RUN_ID.md"

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
  echo "Stable benchmark requires wrk or ab."
  echo "Install wrk with: brew install wrk"
  exit 1
fi

if ! [[ "$REPEATS" =~ ^[0-9]+$ ]] || (( REPEATS < 1 )); then
  echo "BENCH_REPEATS must be a positive integer"
  exit 1
fi

if ! [[ "$WARMUP_SECONDS" =~ ^[0-9]+$ ]] || (( WARMUP_SECONDS < 0 )); then
  echo "BENCH_WARMUP_SECONDS must be a non-negative integer"
  exit 1
fi

if ! [[ "$REQUESTS" =~ ^[0-9]+$ ]] || (( REQUESTS < 1 )); then
  echo "BENCH_REQUESTS must be a positive integer"
  exit 1
fi

if ! [[ "$WARMUP_REQUESTS" =~ ^[0-9]+$ ]] || (( WARMUP_REQUESTS < 0 )); then
  echo "BENCH_WARMUP_REQUESTS must be a non-negative integer"
  exit 1
fi

php -c /dev/null -d extension="$EXT_PATH" "$SERVER_SCRIPT" >"$RESULTS_DIR/server-stable-${PROFILE}-$RUN_ID.log" 2>&1 &
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
  echo "Server failed to start. Check: $RESULTS_DIR/server-stable-${PROFILE}-$RUN_ID.log"
  exit 1
fi

latency_to_ms() {
  awk -v v="$1" 'BEGIN {
    if (v ~ /us$/) { sub(/us$/, "", v); printf "%.6f", v / 1000.0; exit }
    if (v ~ /ms$/) { sub(/ms$/, "", v); printf "%.6f", v + 0; exit }
    if (v ~ /s$/)  { sub(/s$/, "", v);  printf "%.6f", v * 1000.0; exit }
    printf "%.6f", v + 0
  }'
}

median_from_file() {
  local file="$1"
  local count
  count="$(wc -l < "$file" | tr -d ' ')"
  if [[ "$count" == "0" ]]; then
    echo "N/A"
    return
  fi
  sort -n "$file" | awk -v n="$count" '
    {
      vals[NR] = $1
    }
    END {
      if (n % 2 == 1) {
        printf "%.6f", vals[int(n / 2) + 1]
      } else {
        printf "%.6f", (vals[n / 2] + vals[n / 2 + 1]) / 2.0
      }
    }
  '
}

echo "# KislayPHP Stable Benchmark" > "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "- Profile: $PROFILE" >> "$SUMMARY_FILE"
echo "- Tool: $TOOL" >> "$SUMMARY_FILE"
echo "- Host: $HOST:$PORT" >> "$SUMMARY_FILE"
if [[ "$TOOL" == "wrk" ]]; then
  echo "- Duration per run: ${DURATION}s" >> "$SUMMARY_FILE"
  echo "- Threads: $THREADS" >> "$SUMMARY_FILE"
else
  echo "- Requests per run: $REQUESTS" >> "$SUMMARY_FILE"
fi
echo "- Concurrency: $CONCURRENCY" >> "$SUMMARY_FILE"
if [[ "$TOOL" == "wrk" ]]; then
  echo "- Warmup: ${WARMUP_SECONDS}s" >> "$SUMMARY_FILE"
else
  echo "- Warmup requests: $WARMUP_REQUESTS" >> "$SUMMARY_FILE"
fi
echo "- Repeats: $REPEATS" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "| Endpoint | Req/sec (median) | P95 (median) | P99 (median) |" >> "$SUMMARY_FILE"
echo "|---|---:|---:|---:|" >> "$SUMMARY_FILE"

read -r -a ENDPOINTS <<< "$ENDPOINTS_RAW"

for path in "${ENDPOINTS[@]}"; do
  url="http://$HOST:$PORT$path"

  if [[ "$TOOL" == "wrk" ]]; then
    if (( WARMUP_SECONDS > 0 )); then
      wrk -t"$THREADS" -c"$CONCURRENCY" -d"${WARMUP_SECONDS}s" "$url" >/dev/null 2>&1 || true
    fi
  else
    if (( WARMUP_REQUESTS > 0 )); then
      ab -n "$WARMUP_REQUESTS" -c "$CONCURRENCY" "$url" >/dev/null 2>&1 || true
    fi
  fi

  req_file="$RESULTS_DIR/stable-${PROFILE}-${RUN_ID}-$(echo "$path" | tr '/' '_')-req.tmp"
  p95_file="$RESULTS_DIR/stable-${PROFILE}-${RUN_ID}-$(echo "$path" | tr '/' '_')-p95.tmp"
  p99_file="$RESULTS_DIR/stable-${PROFILE}-${RUN_ID}-$(echo "$path" | tr '/' '_')-p99.tmp"
  : > "$req_file"
  : > "$p95_file"
  : > "$p99_file"

  for i in $(seq 1 "$REPEATS"); do
    raw="$RESULTS_DIR/raw-stable-${PROFILE}-${RUN_ID}-$(echo "$path" | tr '/' '_')-run${i}.txt"
    if [[ "$TOOL" == "wrk" ]]; then
      wrk -t"$THREADS" -c"$CONCURRENCY" -d"${DURATION}s" --latency "$url" > "$raw"
      req_sec="$(awk '/Requests\/sec:/{print $2; exit}' "$raw")"
      p95_token="$(awk '/^[[:space:]]*95%/{print $2; exit}' "$raw")"
      p99_token="$(awk '/^[[:space:]]*99%/{print $2; exit}' "$raw")"
    else
      ab -n "$REQUESTS" -c "$CONCURRENCY" "$url" > "$raw"
      req_sec="$(awk '/Requests per second:/{print $4; exit}' "$raw")"
      p95_token="$(awk '/^[[:space:]]*95%/{print $2 "ms"; exit}' "$raw")"
      p99_token="$(awk '/^[[:space:]]*99%/{print $2 "ms"; exit}' "$raw")"
    fi

    if [[ -n "$req_sec" ]]; then
      printf "%.6f\n" "$req_sec" >> "$req_file"
    fi
    if [[ -n "$p95_token" ]]; then
      latency_to_ms "$p95_token" >> "$p95_file"
      echo >> "$p95_file"
    fi
    if [[ -n "$p99_token" ]]; then
      latency_to_ms "$p99_token" >> "$p99_file"
      echo >> "$p99_file"
    fi
  done

  req_median="$(median_from_file "$req_file")"
  p95_median_ms="$(median_from_file "$p95_file")"
  p99_median_ms="$(median_from_file "$p99_file")"

  if [[ "$req_median" != "N/A" ]]; then
    req_median="$(printf "%.2f" "$req_median")"
  fi
  if [[ "$p95_median_ms" != "N/A" ]]; then
    p95_median_ms="$(printf "%.2fms" "$p95_median_ms")"
  fi
  if [[ "$p99_median_ms" != "N/A" ]]; then
    p99_median_ms="$(printf "%.2fms" "$p99_median_ms")"
  fi

  echo "| $path | $req_median | $p95_median_ms | $p99_median_ms |" >> "$SUMMARY_FILE"

  rm -f "$req_file" "$p95_file" "$p99_file"
done

echo "" >> "$SUMMARY_FILE"
echo "Raw outputs:" >> "$SUMMARY_FILE"
for path in "${ENDPOINTS[@]}"; do
  for i in $(seq 1 "$REPEATS"); do
    echo "- raw-stable-${PROFILE}-${RUN_ID}-$(echo "$path" | tr '/' '_')-run${i}.txt" >> "$SUMMARY_FILE"
  done
done

echo "Saved stable summary: $SUMMARY_FILE"
