--TEST--
Kislay Core executes onRequestStart and onRequestEnd hooks for each request
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
?>
--FILE--
<?php
function fail($message) {
    echo "FAIL: {$message}\n";
    exit(1);
}

function reserve_free_port() {
    $socket = @stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    if (!$socket) {
        fail("Unable to reserve free port: {$errstr}");
    }
    $name = stream_socket_get_name($socket, false);
    fclose($socket);
    $parts = explode(':', $name);
    $port = (int) end($parts);
    if ($port <= 0) {
        fail('Reserved port is invalid');
    }
    return $port;
}

function make_request($host, $port, $method, $path) {
    $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
    if (!$fp) {
        fail("Failed to connect: {$errstr}");
    }

    $request = "{$method} {$path} HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
    fwrite($fp, $request);
    $response = stream_get_contents($fp);
    fclose($fp);

    if ($response === false || $response === '') {
        fail('Empty response');
    }

    return $response;
}

$host = '127.0.0.1';
$port = reserve_free_port();
$startCount = 0;
$endCount = 0;

$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->onRequestStart(function ($req, $res) use (&$startCount) {
    $startCount++;
});
$app->onRequestEnd(function ($req, $res) use (&$endCount) {
    $endCount++;
});
$app->get('/health', function ($req, $res) {
    $res->json(['ok' => true], 200);
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);
$response = make_request($host, $port, 'GET', '/health');
$app->stop();

$status = strtok($response, "\r\n");
if ($status === false || strpos($status, '200') === false) {
    fail("Expected 200 response, got: {$status}");
}
if ($startCount !== 1) {
    fail("Expected onRequestStart count=1, got {$startCount}");
}
if ($endCount !== 1) {
    fail("Expected onRequestEnd count=1, got {$endCount}");
}

echo "OK\n";
?>
--EXPECT--
OK
