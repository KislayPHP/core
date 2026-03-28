#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-nts}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXPORT_DIR="$(mktemp -d /tmp/kislay-core-docker-src.XXXXXX)"

cleanup() {
  rm -rf "$EXPORT_DIR"
}
trap cleanup EXIT

case "$MODE" in
  nts)
    PHP_IMAGE="${PHP_IMAGE:-php:8.5-rc-cli}"
    PHP_BIN="php"
    EXTRA_ENV=""
    ;;
  zts)
    PHP_IMAGE="${PHP_IMAGE:-php:8.5-rc-zts}"
    PHP_BIN="php"
    EXTRA_ENV="export KISLAY_ENABLE_ZTS_PARALLEL=1"
    ;;
  *)
    echo "usage: $0 [nts|zts]" >&2
    exit 1
    ;;
esac

REQUESTS="${REQUESTS:-5000}"
CONCURRENCY="${CONCURRENCY:-100}"
WORKERS="${WORKERS:-10}"
HTTP_THREADS="${HTTP_THREADS:-8}"

rsync -a \
  --exclude='.git' \
  --exclude='modules' \
  --exclude='autom4te.cache' \
  --exclude='build' \
  --exclude='bench/results' \
  --exclude='*.loT' \
  --exclude='Makefile' \
  --exclude='Makefile.fragments' \
  --exclude='Makefile.objects' \
  "$ROOT_DIR"/ "$EXPORT_DIR"/

rm -rf \
  "$EXPORT_DIR/autom4te.cache" \
  "$EXPORT_DIR/build" \
  "$EXPORT_DIR/modules" \
  "$EXPORT_DIR/.libs" \
  "$EXPORT_DIR/.deps" \
  "$EXPORT_DIR/third_party/civetweb/src/.libs" \
  "$EXPORT_DIR/third_party/civetweb/src/.deps"
rm -f \
  "$EXPORT_DIR/Makefile" \
  "$EXPORT_DIR/Makefile.fragments" \
  "$EXPORT_DIR/Makefile.objects" \
  "$EXPORT_DIR/config.h" \
  "$EXPORT_DIR/config.log" \
  "$EXPORT_DIR/config.nice" \
  "$EXPORT_DIR/config.status" \
  "$EXPORT_DIR/configure" \
  "$EXPORT_DIR/libtool" \
  "$EXPORT_DIR/kislayphp_extension.la" \
  "$EXPORT_DIR/"*.dep \
  "$EXPORT_DIR/"*.lo \
  "$EXPORT_DIR/"*.la \
  "$EXPORT_DIR/src/runtime/"*.dep \
  "$EXPORT_DIR/src/runtime/"*.lo \
  "$EXPORT_DIR/third_party/civetweb/src/"*.dep \
  "$EXPORT_DIR/third_party/civetweb/src/"*.lo \
  "$EXPORT_DIR/third_party/civetweb/src/"*.la

COPYFILE_DISABLE=1 COPY_EXTENDED_ATTRIBUTES_DISABLE=1 tar \
  --no-xattrs \
  --no-mac-metadata \
  --exclude='.git' \
  --exclude='modules' \
  --exclude='autom4te.cache' \
  --exclude='build' \
  --exclude='bench/results' \
  --exclude='*.loT' \
  --exclude='Makefile' \
  --exclude='Makefile.fragments' \
  --exclude='Makefile.objects' \
  -C "$EXPORT_DIR" \
  -cf - . 2>/dev/null | docker run --rm -i \
  -e REQUESTS="$REQUESTS" \
  -e CONCURRENCY="$CONCURRENCY" \
  -e WORKERS="$WORKERS" \
  -e HTTP_THREADS="$HTTP_THREADS" \
  "$PHP_IMAGE" \
  bash -lc "
    set -euo pipefail
    dump_logs() {
      status=\$?
      echo '=== docker benchmark failure ===' >&2
      for log in /tmp/apt-update.log /tmp/apt-install.log /tmp/phpize.log /tmp/configure.log /tmp/make.log /tmp/server.log; do
        if [[ -f \$log ]]; then
          echo \"--- \$log ---\" >&2
          tail -n 200 \$log >&2 || true
        fi
      done
      exit \$status
    }
    trap dump_logs ERR
    export DEBIAN_FRONTEND=noninteractive
    apt-get update >/tmp/apt-update.log
    apt-get install -y --no-install-recommends \
      apache2-utils \
      autoconf \
      build-essential \
      curl \
      libcurl4-openssl-dev \
      libllhttp-dev \
      libssl-dev \
      libuv1-dev \
      pkg-config >/tmp/apt-install.log

    rm -rf /work
    mkdir -p /work
    tar -xf - -C /work
    cd /work

    phpize >/tmp/phpize.log
    ./configure >/tmp/configure.log 2>&1
    make -j\"\$(nproc)\" >/tmp/make.log 2>&1

    export BENCH_PORT=9090
    export KISLAYPHP_WORKERS=\"$WORKERS\"
    export KISLAYPHP_HTTP_THREADS=\"$HTTP_THREADS\"
    export KISLAYPHP_HTTP_LOG=0
    export KISLAYPHP_HTTP_REQUEST_ID=1
    export KISLAYPHP_HTTP_TRACE=1
    ${EXTRA_ENV}

    ${PHP_BIN} -n -d extension=/work/modules/kislayphp_extension.so /work/bench/benchmark_server.php >/tmp/server.log 2>&1 &
    SERVER_PID=\$!
    cleanup() {
      kill \$SERVER_PID >/dev/null 2>&1 || true
      wait \$SERVER_PID 2>/dev/null || true
    }
    trap cleanup EXIT

    for _ in \$(seq 1 40); do
      if curl -sf http://127.0.0.1:9090/health >/dev/null; then
        break
      fi
      sleep 0.25
    done

    echo \"=== $MODE plaintext ===\"
    ab -n \"$REQUESTS\" -c \"$CONCURRENCY\" http://127.0.0.1:9090/plaintext \
      | sed -n '/Requests per second/p;/Failed requests/p;/50%/p;/95%/p'

    echo \"=== $MODE json ===\"
    ab -l -n \"$REQUESTS\" -c \"$CONCURRENCY\" http://127.0.0.1:9090/json \
      | sed -n '/Requests per second/p;/Failed requests/p;/50%/p;/95%/p'
  "
