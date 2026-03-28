<?php

if (!extension_loaded('kislayphp_extension')) {
    fwrite(STDERR, "kislayphp_extension is not loaded\n");
    exit(1);
}

function kislay_bench_health($req, $res): void
{
    $res->send('ok');
}

function kislay_bench_plaintext($req, $res): void
{
    $res->send('hello from kislayphp');
}

function kislay_bench_json($req, $res): void
{
    $res->json([
        'status' => 'ok',
        'service' => 'kislay-bench',
        'ts' => microtime(true),
    ]);
}

function kislay_bench_async_task(): int
{
    return 42;
}

function kislay_bench_async($req, $res): void
{
    async('kislay_bench_async_task');
    $res->send('ok');
}

function kislay_bench_file($req, $res): void
{
    global $bigFile;
    $res->sendFile($bigFile, 'application/octet-stream');
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

$app->get('/health', 'kislay_bench_health');
$app->get('/plaintext', 'kislay_bench_plaintext');
$app->get('/json', 'kislay_bench_json');
$app->get('/async', 'kislay_bench_async');
$app->get('/file', 'kislay_bench_file');

fwrite(STDOUT, "Benchmark server running on http://{$host}:{$port}\n");
$app->listen($host, $port);
