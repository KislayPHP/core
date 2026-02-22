#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STACK_DIR="$ROOT/examples/microservice-stack"
PID_DIR="$STACK_DIR/run/pids"

if [[ -f "$STACK_DIR/.env" ]]; then
  # shellcheck disable=SC1090
  source "$STACK_DIR/.env"
fi

REGISTRY_PORT="${REGISTRY_PORT:-9090}"
GATEWAY_PORT="${GATEWAY_PORT:-9008}"

for name in registry user order gateway; do
  pid_file="$PID_DIR/$name.pid"
  if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
    echo "$name: running (pid $(cat "$pid_file"))"
  else
    echo "$name: stopped"
  fi
done

echo
for url in \
  "http://127.0.0.1:${REGISTRY_PORT}/health" \
  "http://127.0.0.1:${REGISTRY_PORT}/v1/services" \
  "http://127.0.0.1:${GATEWAY_PORT}/api/users" \
  "http://127.0.0.1:${GATEWAY_PORT}/api/orders"; do
  code=$(curl -sS -o /tmp/kislay_stack_status.$$ -w '%{http_code}' "$url" || true)
  echo "$url -> HTTP $code"
  head -c 180 /tmp/kislay_stack_status.$$ || true
  echo
  echo
 done

rm -f /tmp/kislay_stack_status.$$
