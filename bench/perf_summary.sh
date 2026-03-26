#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXT_PATH="$ROOT_DIR/modules/kislayphp_extension.so"
SERVER_SCRIPT="$ROOT_DIR/bench/benchmark_server.php"
CLIENT_SCRIPT="$ROOT_DIR/bench/simple_bench_client.php"

export KISLAYPHP_HTTP_LOG=0
export KISLAYPHP_HTTP_REQUEST_ID=0
export KISLAYPHP_HTTP_TRACE=0
export KISLAYPHP_HTTP_THREADS="${BENCH_HTTP_THREADS:-8}"
export KISLAYPHP_WORKERS="${BENCH_WORKERS:-10}"

run_summary() {
    local server_type=$1
    echo "================================================="
    echo "Kislay Core Performance: $server_type"
    echo "================================================="

    # Start server with specified type
    php -n -d extension="$EXT_PATH" "$SERVER_SCRIPT" > /dev/null 2>&1 &
    SERVER_PID=$!
    
    # Allow some time for the server to bind
    sleep 2
    
    # Set server type via curl (if we had an API) or just rely on the fact that 
    # benchmark_server.php could be modified. 
    # Actually, we can just pass it as an option if we modified the script, 
    # but let's just use the env var for server type if supported, or modify the script.
    
    cleanup() {
      kill "$SERVER_PID" || true
      wait "$SERVER_PID" 2>/dev/null || true
    }
    trap cleanup EXIT

    echo "Warming up (10k requests)..."
    php "$CLIENT_SCRIPT" "http://127.0.0.1:9090/plaintext" 100 10000 > /dev/null
    echo "Warm-up complete."
    echo ""

    run_test() {
        local name=$1
        local url=$2
        echo "Testing $name..."
        php "$CLIENT_SCRIPT" "$url" 100 20000 | grep -E "Requests per second|Average latency"
        echo ""
    }

    run_test "Plaintext" "http://127.0.0.1:9090/plaintext"
    run_test "JSON" "http://127.0.0.1:9090/json"
    
    kill "$SERVER_PID"
    trap - EXIT
}

# First test default (CivetWeb)
run_summary "CivetWeb"

# Then test LibUv (we'll modify the benchmark server to use libuv for this run)
echo "Switching to LibUv..."
sed -i '' "s/new Kislay\\\\Core\\\\App()/new Kislay\\\\Core\\\\App(['server_type' => 'libuv'])/" "$SERVER_SCRIPT"
run_summary "LibUv"
# Revert change
sed -i '' "s/new Kislay\\\\Core\\\\App(\['server_type' => 'libuv'\])/new Kislay\\\\Core\\\\App()/" "$SERVER_SCRIPT"
