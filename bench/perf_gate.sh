#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$ROOT_DIR/bench/results"
PROFILE="${PERF_PROFILE:-${BENCH_PROFILE:-default}}"

REPORT_FILE="${1:-}"
if [[ -z "$REPORT_FILE" ]]; then
  REPORT_FILE="$(ls -1t "$RESULTS_DIR"/benchmark-stable-delta-${PROFILE}-*.md 2>/dev/null | head -n 1 || true)"
fi

if [[ -z "$REPORT_FILE" || ! -f "$REPORT_FILE" ]]; then
  echo "No stable delta report found for profile '$PROFILE'. Run: make bench-stable-delta"
  exit 1
fi

MAX_REQ_REGRESSION="${PERF_GATE_MAX_REQ_REGRESSION_PCT:-5}"
MAX_P95_REGRESSION="${PERF_GATE_MAX_P95_REGRESSION_PCT:-20}"
MAX_P99_REGRESSION="${PERF_GATE_MAX_P99_REGRESSION_PCT:-30}"
EXCLUDE_ENDPOINTS_RAW="${PERF_GATE_EXCLUDE_ENDPOINTS:-}"
SOFT_FAIL="${PERF_GATE_SOFT_FAIL:-0}"

declare -a EXCLUDE_ENDPOINTS=()
if [[ -n "$EXCLUDE_ENDPOINTS_RAW" ]]; then
  read -r -a EXCLUDE_ENDPOINTS <<< "$EXCLUDE_ENDPOINTS_RAW"
fi

is_excluded_endpoint() {
  local endpoint="$1"
  if (( ${#EXCLUDE_ENDPOINTS[@]} == 0 )); then
    return 1
  fi
  for excluded in "${EXCLUDE_ENDPOINTS[@]}"; do
    if [[ "$endpoint" == "$excluded" ]]; then
      return 0
    fi
  done
  return 1
}

trim() {
  local v="$1"
  v="${v#${v%%[![:space:]]*}}"
  v="${v%${v##*[![:space:]]}}"
  echo "$v"
}

to_number() {
  local raw
  raw="$(trim "$1")"
  raw="${raw%%%}"
  if [[ "$raw" == "N/A" || -z "$raw" ]]; then
    echo ""
    return
  fi
  if [[ "$raw" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
    echo "$raw"
    return
  fi
  echo ""
}

is_gt() {
  local left="$1"
  local right="$2"
  awk -v l="$left" -v r="$right" 'BEGIN { exit !(l > r) }'
}

failures=0

while IFS='|' read -r _ endpoint old_req new_req req_delta p95_delta p99_delta _rest; do
  endpoint="$(trim "$endpoint")"
  [[ -z "$endpoint" || "$endpoint" == "Endpoint" || "$endpoint" == "---" ]] && continue
  if is_excluded_endpoint "$endpoint"; then
    continue
  fi

  req_delta_num="$(to_number "$req_delta")"
  p95_delta_num="$(to_number "$p95_delta")"
  p99_delta_num="$(to_number "$p99_delta")"

  if [[ -n "$req_delta_num" ]]; then
    req_regression="0"
    if awk -v d="$req_delta_num" 'BEGIN { exit !(d < 0) }'; then
      req_regression="$(awk -v d="$req_delta_num" 'BEGIN { printf "%.6f", -d }')"
    fi
    if is_gt "$req_regression" "$MAX_REQ_REGRESSION"; then
      echo "FAIL [$endpoint] Req/sec regression ${req_regression}% exceeds max ${MAX_REQ_REGRESSION}%"
      failures=$((failures + 1))
    fi
  fi

  if [[ -n "$p95_delta_num" ]] && is_gt "$p95_delta_num" "$MAX_P95_REGRESSION"; then
    echo "FAIL [$endpoint] P95 regression ${p95_delta_num}% exceeds max ${MAX_P95_REGRESSION}%"
    failures=$((failures + 1))
  fi

  if [[ -n "$p99_delta_num" ]] && is_gt "$p99_delta_num" "$MAX_P99_REGRESSION"; then
    echo "FAIL [$endpoint] P99 regression ${p99_delta_num}% exceeds max ${MAX_P99_REGRESSION}%"
    failures=$((failures + 1))
  fi
done < <(awk '/^\|[[:space:]]*\//{print}' "$REPORT_FILE")

if (( failures > 0 )); then
  if [[ "$SOFT_FAIL" == "1" ]]; then
    echo "Performance gate soft-fail: $failures issue(s) detected."
    echo "Report: $REPORT_FILE"
    exit 0
  fi
  echo "Performance gate failed with $failures issue(s)."
  echo "Report: $REPORT_FILE"
  exit 1
fi

echo "Performance gate passed."
echo "Report: $REPORT_FILE"
