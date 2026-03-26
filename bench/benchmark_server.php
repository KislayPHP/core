<?php

if (!extension_loaded('kislayphp_extension')) {
    fwrite(STDERR, "kislayphp_extension is not loaded\n");
    exit(1);
}

$port = (int) (getenv('BENCH_PORT') ?: 9090);
$host = getenv('BENCH_HOST') ?: '127.0.0.1';
$benchWorkers = (int) (getenv('BENCH_WORKERS') ?: 10);
$benchThreads = (int) (getenv('BENCH_HTTP_THREADS') ?: 8);

$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('num_threads', $benchThreads);
if ($benchWorkers > 1) {
    $app->setOption('workers', $benchWorkers);
}

$bigFile = __DIR__ . '/static-1mb.bin';
if (!file_exists($bigFile)) {
    $payload = random_bytes(1024);
    $chunks = 1024;
    $fp = fopen($bigFile, 'wb');
    if ($fp) {
        for ($i = 0; $i < $chunks; $i++) {
            fwrite($fp, $payload);
        }
        fclose($fp);
    }
}

$app->get('/health', function ($req, $res) {
    $res->send('ok');
});

$app->get('/plaintext', function ($req, $res) {
    $res->send('hello from kislayphp');
});

$jsonPayload = json_encode([
    'status' => 'ok',
    'service' => 'kislay-bench',
    'ts' => 1700000000.123456,
], JSON_UNESCAPED_SLASHES);

$app->get('/json', function ($req, $res) use ($jsonPayload) {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($jsonPayload);
});

$app->get('/async', function ($req, $res) {
    async(function() {
        return 42;
    });
    $res->send('ok');
});

$app->get('/file', function ($req, $res) use ($bigFile) {
    $res->sendFile($bigFile, 'application/octet-stream');
});

fwrite(STDOUT, "Benchmark server running on http://{$host}:{$port}\n");
$app->listen($host, $port);
