<?php
/**
 * KislayPHP — Pre-fork benchmark server
 *
 * Spawns PREFORK_WORKERS child processes, each running its own CivetWeb
 * instance on the same port.  The OS distributes incoming connections across
 * workers via SO_REUSEPORT (enabled automatically by CivetWeb on both macOS
 * and Linux).  Each worker has its own independent PHP execution thread, so
 * there is zero lock contention between workers — this is the NTS equivalent
 * of a ZTS multi-thread deployment.
 *
 * Usage (run_prefork.sh handles this automatically):
 *   PREFORK_WORKERS=8 BENCH_PORT=9595 \
 *   php -n -d extension=.../kislayphp_extension.so bench/prefork_server.php
 */

if (!extension_loaded('kislayphp_extension')) {
    fwrite(STDERR, "[prefork_server] ERROR: kislayphp_extension not loaded\n");
    exit(1);
}
if (!function_exists('pcntl_fork')) {
    fwrite(STDERR, "[prefork_server] ERROR: pcntl extension not available\n");
    fwrite(STDERR, "  Install: brew install php && php -m | grep pcntl\n");
    exit(1);
}

$workers  = max(1, (int)(getenv('PREFORK_WORKERS') ?: 8));
$host     = getenv('BENCH_HOST') ?: '127.0.0.1';
$port     = (int)(getenv('BENCH_PORT') ?: 9595);
$threads  = max(1, (int)(getenv('BENCH_HTTP_THREADS') ?: 2)); // CivetWeb threads per worker
$logFile  = getenv('BENCH_LOG') ?: '/tmp/kislay_prefork.log';

// ── Pre-built payloads (allocated once in parent, copied into each worker) ────
$jsonPayload = '{"status":"ok","service":"kislay-bench","ts":1700000000.123}';
$staticDir   = __DIR__ . '/static';
$bigFile     = $staticDir . '/static-1mb.bin';

if (!file_exists($bigFile)) {
    @mkdir($staticDir, 0755, true);
    $fp = fopen($bigFile, 'wb');
    if ($fp) {
        for ($i = 0; $i < 1024; $i++) { fwrite($fp, str_repeat('A', 1024)); }
        fclose($fp);
    }
}

// ── Build the app BEFORE forking so all workers share the same route table ───
$app = new Kislay\Core\App();
$app->setOption('log',         false);
$app->setOption('num_threads', $threads);

$app->get('/health',    function ($req, $res): void {
    $res->send('ok');
});
$app->get('/plaintext', function ($req, $res): void {
    $res->send('Hello from KislayPHP');
});
$app->get('/json', function ($req, $res) use ($jsonPayload): void {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($jsonPayload);
});
$app->get('/file', function ($req, $res) use ($bigFile): void {
    $res->sendFile($bigFile, 'application/octet-stream');
});

// ── Fork workers ──────────────────────────────────────────────────────────────
$pids = [];

for ($i = 1; $i < $workers; $i++) {
    $pid = pcntl_fork();

    if ($pid === -1) {
        fwrite(STDERR, "[prefork_server] pcntl_fork failed at worker $i\n");
        break;
    }

    if ($pid === 0) {
        // ── Child process ────────────────────────────────────────────────────
        // Each child starts its own independent CivetWeb + PHP runtime.
        // CivetWeb sets SO_REUSEPORT so all workers bind the same port.
        fwrite(STDOUT, "[worker $i  pid=" . getmypid() . "] http://{$host}:{$port}\n");
        $app->listen($host, $port);
        exit(0);
    }

    $pids[] = $pid;
}

// ── Master also listens (worker 0) ────────────────────────────────────────────
fwrite(STDOUT, "[master    pid=" . getmypid() . "] http://{$host}:{$port}  ($workers workers × $threads threads)\n");
$app->listen($host, $port);

// Reached only on server stop — reap children
foreach ($pids as $pid) {
    pcntl_waitpid($pid, $status);
}
