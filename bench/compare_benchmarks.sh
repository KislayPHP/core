#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$ROOT_DIR/bench/results"

NEW_FILE="${1:-}"
OLD_FILE="${2:-}"

if [[ -z "$NEW_FILE" || -z "$OLD_FILE" ]]; then
  files_list="$(ls -1t "$RESULTS_DIR"/benchmark-matrix-*.md 2>/dev/null || true)"
  newest="$(printf '%s\n' "$files_list" | sed -n '1p')"
  previous="$(printf '%s\n' "$files_list" | sed -n '2p')"
  if [[ -z "$newest" || -z "$previous" ]]; then
    echo "Need two benchmark matrix files."
    echo "Run: ./bench/run_benchmark_matrix.sh (twice)"
    exit 1
  fi
  NEW_FILE="$newest"
  OLD_FILE="$previous"
fi

if [[ ! -f "$NEW_FILE" || ! -f "$OLD_FILE" ]]; then
  echo "Benchmark files not found."
  echo "NEW: $NEW_FILE"
  echo "OLD: $OLD_FILE"
  exit 1
fi

parse_req() {
  local file="$1" endpoint="$2"
  awk -F'|' -v ep="$endpoint" '
    $0 ~ "\\|[[:space:]]*"ep"[[:space:]]*\\|" {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $3);
      print $3;
      exit
    }
  ' "$file"
}

parse_lat() {
  local file="$1" endpoint="$2" col="$3"
  awk -F'|' -v ep="$endpoint" -v c="$col" '
    $0 ~ "\\|[[:space:]]*"ep"[[:space:]]*\\|" {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $c);
      print $c;
      exit
    }
  ' "$file"
}

to_ms() {
  local v="${1:-N/A}"
  if [[ "$v" == "N/A" || -z "$v" ]]; then
    echo ""
    return
  fi
  if [[ "$v" =~ ^([0-9]+(\.[0-9]+)?)ms$ ]]; then
    echo "${BASH_REMATCH[1]}"
    return
  fi
  if [[ "$v" =~ ^([0-9]+(\.[0-9]+)?)us$ ]]; then
    awk -v n="${BASH_REMATCH[1]}" 'BEGIN { printf "%.6f", (n/1000.0) }'
    return
  fi
  if [[ "$v" =~ ^([0-9]+(\.[0-9]+)?)s$ ]]; then
    awk -v n="${BASH_REMATCH[1]}" 'BEGIN { printf "%.6f", (n*1000.0) }'
    return
  fi
  if [[ "$v" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "$v"
    return
  fi
  echo ""
}

pct_change() {
  local old="$1" new="$2"
  if [[ -z "$old" || -z "$new" ]]; then
    echo "N/A"
    return
  fi
  awk -v o="$old" -v n="$new" 'BEGIN {
    if (o == 0) { print "N/A"; exit }
    p=((n-o)/o)*100.0;
    printf "%.2f%%", p;
  }'
}

latency_delta() {
  local old_raw="$1" new_raw="$2"
  local old_ms new_ms
  old_ms="$(to_ms "$old_raw")"
  new_ms="$(to_ms "$new_raw")"
  if [[ -z "$old_ms" || -z "$new_ms" ]]; then
    echo "N/A"
    return
  fi
  awk -v o="$old_ms" -v n="$new_ms" 'BEGIN {
    if (o == 0) { print "N/A"; exit }
    p=((n-o)/o)*100.0;
    printf "%.2f%%", p;
  }'
}

run_id="$(date +%Y%m%d-%H%M%S)"
out_file="$RESULTS_DIR/benchmark-delta-$run_id.md"

endpoints=("/plaintext" "/json" "/file")

echo "# KislayPHP Benchmark Delta" > "$out_file"
echo "" >> "$out_file"
echo "- Baseline: $(basename "$OLD_FILE")" >> "$out_file"
echo "- Candidate: $(basename "$NEW_FILE")" >> "$out_file"
echo "" >> "$out_file"
echo "| Endpoint | Req/sec (old) | Req/sec (new) | Req/sec Δ | P95 Δ | P99 Δ |" >> "$out_file"
echo "|---|---:|---:|---:|---:|---:|" >> "$out_file"

for ep in "${endpoints[@]}"; do
  old_req="$(parse_req "$OLD_FILE" "$ep")"
  new_req="$(parse_req "$NEW_FILE" "$ep")"
  old_p95="$(parse_lat "$OLD_FILE" "$ep" 4)"
  new_p95="$(parse_lat "$NEW_FILE" "$ep" 4)"
  old_p99="$(parse_lat "$OLD_FILE" "$ep" 5)"
  new_p99="$(parse_lat "$NEW_FILE" "$ep" 5)"

  req_delta="$(pct_change "$old_req" "$new_req")"
  p95_delta="$(latency_delta "$old_p95" "$new_p95")"
  p99_delta="$(latency_delta "$old_p99" "$new_p99")"

  echo "| $ep | ${old_req:-N/A} | ${new_req:-N/A} | $req_delta | $p95_delta | $p99_delta |" >> "$out_file"
done

echo "" >> "$out_file"
echo "Interpretation: positive Req/sec is better; negative latency delta is better." >> "$out_file"

echo "Saved delta report: $out_file"
