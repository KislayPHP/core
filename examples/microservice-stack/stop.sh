#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PID_DIR="$ROOT/examples/microservice-stack/run/pids"

if [[ ! -d "$PID_DIR" ]]; then
  echo "No running stack state found."
  exit 0
fi

for name in gateway order user registry; do
  pid_file="$PID_DIR/$name.pid"
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file")"
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" || true
      echo "stopped $name (pid $pid)"
    else
      echo "$name not running (stale pid $pid)"
    fi
    rm -f "$pid_file"
  fi
done
