#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$ROOT/.." && pwd)}"
STACK_DIR="$ROOT/examples/microservice-stack"
RUN_DIR="$STACK_DIR/run"
PID_DIR="$RUN_DIR/pids"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$PID_DIR" "$LOG_DIR"

if [[ -f "$STACK_DIR/.env" ]]; then
  # shellcheck disable=SC1090
  source "$STACK_DIR/.env"
fi

REGISTRY_HOST="${REGISTRY_HOST:-0.0.0.0}"
REGISTRY_PORT="${REGISTRY_PORT:-9090}"
GATEWAY_HOST="${GATEWAY_HOST:-0.0.0.0}"
GATEWAY_PORT="${GATEWAY_PORT:-9008}"

USER_SERVICE_NAME="${USER_SERVICE_NAME:-user-service}"
USER_SERVICE_PORT="${USER_SERVICE_PORT:-9101}"
USER_SERVICE_ROUTE="${USER_SERVICE_ROUTE:-/api/users}"
ORDER_SERVICE_NAME="${ORDER_SERVICE_NAME:-order-service}"
ORDER_SERVICE_PORT="${ORDER_SERVICE_PORT:-9102}"
ORDER_SERVICE_ROUTE="${ORDER_SERVICE_ROUTE:-/api/orders}"

REGISTRY_URL="${REGISTRY_URL:-http://127.0.0.1:${REGISTRY_PORT}}"
GATEWAY_FALLBACK_TARGET="${GATEWAY_FALLBACK_TARGET:-}"

CORE_SO="${CORE_SO:-$ROOT/modules/kislayphp_extension.so}"
DISCOVERY_SO="${DISCOVERY_SO:-$WORKSPACE_ROOT/kislayphp_discovery/modules/kislayphp_discovery.so}"
GATEWAY_SO="${GATEWAY_SO:-$WORKSPACE_ROOT/kislayphp_gateway/modules/kislayphp_gateway.so}"

DISCOVERY_EXAMPLES="${DISCOVERY_EXAMPLES:-$WORKSPACE_ROOT/kislayphp_discovery/examples/standalone_registry}"
REGISTRY_SCRIPT="$DISCOVERY_EXAMPLES/registry_server.php"
SERVICE_SCRIPT="$DISCOVERY_EXAMPLES/service_example.php"
GATEWAY_SCRIPT="$DISCOVERY_EXAMPLES/gateway_example.php"

if [[ ! -f "$REGISTRY_SCRIPT" || ! -f "$SERVICE_SCRIPT" || ! -f "$GATEWAY_SCRIPT" ]]; then
  echo "Missing discovery example scripts under $DISCOVERY_EXAMPLES"
  exit 1
fi

build_php_cmd() {
  local -a cmd=(php)
  local so
  for so in "$@"; do
    if [[ -n "$so" && -f "$so" ]]; then
      cmd+=("-d" "extension=$so")
    fi
  done
  printf '%q ' "${cmd[@]}"
}

PHP_REGISTRY_BASE="$(build_php_cmd "$CORE_SO" "$DISCOVERY_SO")"
PHP_SERVICE_BASE="$PHP_REGISTRY_BASE"
PHP_GATEWAY_BASE="$(build_php_cmd "$DISCOVERY_SO" "$GATEWAY_SO")"

start_proc() {
  local name="$1"
  shift
  local pid_file="$PID_DIR/$name.pid"
  local log_file="$LOG_DIR/$name.log"

  if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
    echo "$name already running (pid $(cat "$pid_file"))"
    return 0
  fi

  nohup "$@" >"$log_file" 2>&1 &
  echo "$!" >"$pid_file"
  echo "started $name (pid $!)"
}

start_proc registry env \
  REGISTRY_HOST="$REGISTRY_HOST" REGISTRY_PORT="$REGISTRY_PORT" \
  bash -lc "$PHP_REGISTRY_BASE \"$REGISTRY_SCRIPT\""

start_proc user env \
  SERVICE_NAME="$USER_SERVICE_NAME" SERVICE_PORT="$USER_SERVICE_PORT" \
  SERVICE_URL="http://127.0.0.1:${USER_SERVICE_PORT}" SERVICE_ROUTE="$USER_SERVICE_ROUTE" \
  REGISTRY_URL="$REGISTRY_URL" \
  bash -lc "$PHP_SERVICE_BASE \"$SERVICE_SCRIPT\""

start_proc order env \
  SERVICE_NAME="$ORDER_SERVICE_NAME" SERVICE_PORT="$ORDER_SERVICE_PORT" \
  SERVICE_URL="http://127.0.0.1:${ORDER_SERVICE_PORT}" SERVICE_ROUTE="$ORDER_SERVICE_ROUTE" \
  REGISTRY_URL="$REGISTRY_URL" \
  bash -lc "$PHP_SERVICE_BASE \"$SERVICE_SCRIPT\""

start_proc gateway env \
  REGISTRY_URL="$REGISTRY_URL" GATEWAY_HOST="$GATEWAY_HOST" GATEWAY_PORT="$GATEWAY_PORT" \
  GATEWAY_FALLBACK_TARGET="$GATEWAY_FALLBACK_TARGET" \
  bash -lc "$PHP_GATEWAY_BASE \"$GATEWAY_SCRIPT\""

sleep 2

echo
echo "Stack started"
echo "registry: $REGISTRY_URL/health"
echo "gateway:  http://127.0.0.1:${GATEWAY_PORT}/api/users"
echo "gateway:  http://127.0.0.1:${GATEWAY_PORT}/api/orders"
echo "logs:     $LOG_DIR"
