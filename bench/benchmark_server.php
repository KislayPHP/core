<?php

if (!extension_loaded('kislayphp_extension')) {
    fwrite(STDERR, "kislayphp_extension is not loaded\n");
    exit(1);
}

$port = (int) (getenv('BENCH_PORT') ?: 9090);
$host = getenv('BENCH_HOST') ?: '127.0.0.1';

$app = new Kislay\Core\App();

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

$app->get('/json', function ($req, $res) {
    $res->json([
        'status' => 'ok',
        'service' => 'kislay-bench',
        'ts' => microtime(true),
    ]);
});

$app->get('/file', function ($req, $res) use ($bigFile) {
    $res->sendFile($bigFile, 'application/octet-stream');
});

fwrite(STDOUT, "Benchmark server running on http://{$host}:{$port}\n");
$app->listen($host, $port);
