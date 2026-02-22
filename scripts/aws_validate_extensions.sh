#!/usr/bin/env bash
set -euo pipefail

KEY_PATH="${1:-/Users/dhruvraj/Downloads/firstkis.pem}"
HOST="${2:-ec2-54-242-252-137.compute-1.amazonaws.com}"
USER="${3:-ubuntu}"

if [[ ! -f "$KEY_PATH" ]]; then
  echo "Missing key: $KEY_PATH"
  exit 1
fi

chmod 400 "$KEY_PATH"

echo "Connecting to $USER@$HOST"
ssh -i "$KEY_PATH" -o StrictHostKeyChecking=no "$USER@$HOST" 'bash -s' <<'EOS'
set -euo pipefail

WORK=/home/ubuntu/kislay-main-qa
PHP_CONFIG=$(command -v php-config)

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

repos=(
  "https:https://github.com/KislayPHP/core.git"
  "kislayphp_gateway:https://github.com/KislayPHP/gateway.git"
  "kislayphp_discovery:https://github.com/KislayPHP/discovery.git"
  "kislayphp_queue:https://github.com/KislayPHP/queue.git"
  "kislayphp_metrics:https://github.com/KislayPHP/metrics.git"
  "kislayphp_config:https://github.com/KislayPHP/config.git"
  "kislay_socket:https://github.com/KislayPHP/eventbus.git"
)

for entry in "${repos[@]}"; do
  name="${entry%%:*}"
  url="${entry#*:}"
  echo "=== clone $name ==="
  git clone --depth=1 "$url" "$name"
  echo "=== build $name ==="
  (
    cd "$name"
    phpize >/tmp/${name}_phpize.log 2>&1
    ./configure --with-php-config="$PHP_CONFIG" >/tmp/${name}_configure.log 2>&1
    make -j2 >/tmp/${name}_make.log 2>&1
  )
  echo "built $name"
done

CORE_SO="$WORK/https/modules/kislayphp_extension.so"
GW_SO="$WORK/kislayphp_gateway/modules/kislayphp_gateway.so"
DISC_SO="$WORK/kislayphp_discovery/modules/kislayphp_discovery.so"
Q_SO="$WORK/kislayphp_queue/modules/kislayphp_queue.so"
M_SO="$WORK/kislayphp_metrics/modules/kislayphp_metrics.so"
C_SO="$WORK/kislayphp_config/modules/kislayphp_config.so"
E_SO="$WORK/kislay_socket/modules/kislayphp_eventbus.so"

for so in "$CORE_SO" "$GW_SO" "$DISC_SO" "$Q_SO" "$M_SO" "$C_SO" "$E_SO"; do
  echo "=== load $so ==="
  php -n -d extension="$so" -r 'echo "ok\n";'
done

echo "=== api smoke ==="
php -n -d extension="$Q_SO" -r '$q=new Kislay\Queue\Queue(); $q->enqueue("jobs", ["id"=>1]); $m=$q->dequeue("jobs"); echo "queue=".(is_array($m)?"ok":"fail")."\n";'
php -n -d extension="$M_SO" -r '$m=new Kislay\Metrics\Metrics(); $m->inc("requests",2); $m->dec("requests",1); echo "metrics=".$m->get("requests")."\n";'
php -n -d extension="$C_SO" -r '$c=new Kislay\Config\ConfigClient(); $c->set("app.name","kislay"); echo "config=".$c->get("app.name")."\n";'
php -n -d extension="$E_SO" -r '$s=new Kislay\EventBus\Server(); echo "eventbus=".(is_object($s)?"ok":"fail")."\n";'

EX_DIR="$WORK/kislayphp_discovery/examples/standalone_registry"
RUN=/home/ubuntu/kislay-main-qa/validation
mkdir -p "$RUN"

# Clean up previous validation processes.
pkill -f "$EX_DIR/registry_server.php" || true
pkill -f "$EX_DIR/service_example.php" || true
pkill -f "$EX_DIR/gateway_example.php" || true
sleep 1

REG_URL="http://127.0.0.1:23090"
nohup env REGISTRY_HOST=0.0.0.0 REGISTRY_PORT=23090 php -d extension="$CORE_SO" -d extension="$DISC_SO" "$EX_DIR/registry_server.php" >"$RUN/registry.log" 2>&1 &
nohup env SERVICE_NAME=user-service SERVICE_PORT=23101 SERVICE_URL=http://127.0.0.1:23101 SERVICE_ROUTE=/api/users REGISTRY_URL="$REG_URL" php -d extension="$CORE_SO" -d extension="$DISC_SO" "$EX_DIR/service_example.php" >"$RUN/user.log" 2>&1 &
nohup env SERVICE_NAME=order-service SERVICE_PORT=23102 SERVICE_URL=http://127.0.0.1:23102 SERVICE_ROUTE=/api/orders REGISTRY_URL="$REG_URL" php -d extension="$CORE_SO" -d extension="$DISC_SO" "$EX_DIR/service_example.php" >"$RUN/order.log" 2>&1 &
nohup env REGISTRY_URL="$REG_URL" GATEWAY_HOST=0.0.0.0 GATEWAY_PORT=23008 php -d extension="$DISC_SO" -d extension="$GW_SO" "$EX_DIR/gateway_example.php" >"$RUN/gateway.log" 2>&1 &

for _ in {1..30}; do
  if curl -fsS -m 2 "$REG_URL/health" >/dev/null 2>&1 && curl -fsS -m 2 "http://127.0.0.1:23008/api/users" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

echo "=== communication smoke ==="
echo -n "registry: "
curl -sS -m 3 "$REG_URL/v1/services" || true
echo

echo -n "gateway users: "
curl -sS -m 3 "http://127.0.0.1:23008/api/users" || true
echo

echo -n "gateway orders: "
curl -sS -m 3 "http://127.0.0.1:23008/api/orders" || true
echo

echo "=== validation complete ==="
EOS
