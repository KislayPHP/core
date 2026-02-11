<?php
function require_extension($name) {
    if (!extension_loaded($name)) {
        fwrite(STDERR, "Missing extension: {$name}\n");
        exit(1);
    }
}

require_extension('kislayphp_extension');

if (!function_exists('pcntl_fork') || !function_exists('posix_kill')) {
    fwrite(STDERR, "pcntl/posix not available; run manually in two terminals.\n");
    exit(0);
}

$host = '127.0.0.1';
$port = 18999;

$pid = pcntl_fork();
if ($pid === -1) {
    fwrite(STDERR, "Failed to fork.\n");
    exit(1);
}

if ($pid === 0) {
    $app = new KislayPHP\Core\App();
    $app->get('/ok', function ($req, $res) {
        $res->json(['ok' => true], 200);
    });
    $app->listen($host, $port);
    exit(0);
}

usleep(200000);

$fp = fsockopen($host, $port, $errno, $errstr, 2.0);
if (!$fp) {
    fwrite(STDERR, "Failed to connect: {$errstr}\n");
    posix_kill($pid, SIGTERM);
    pcntl_waitpid($pid, $status);
    exit(1);
}

$request = "GET /ok?empty=&=bad&x=1 HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
fwrite($fp, $request);
$response = stream_get_contents($fp);
fclose($fp);

$ok = false;
if ($response !== false && $response !== '') {
    $first_line = strtok($response, "\r\n");
    if ($first_line !== false && strpos($first_line, '200') !== false) {
        $ok = true;
    }
}

posix_kill($pid, SIGTERM);
pcntl_waitpid($pid, $status);

if (!$ok) {
    fwrite(STDERR, "Unexpected response:\n{$response}\n");
    exit(1);
}

fwrite(STDOUT, "OK\n");
