--TEST--
Kislay Core returns 500 for route exceptions and 404 for missing routes
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

$app = new Kislay\Core\App();
$app->get('/boom', function ($req, $res) {
    throw new Exception('boom');
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

$boomResponse = make_request($host, $port, 'GET', '/boom');
$missingResponse = make_request($host, $port, 'GET', '/missing');

$app->stop();

$boomStatus = strtok($boomResponse, "\r\n");
if ($boomStatus === false || strpos($boomStatus, '500') === false) {
    fail("Expected 500 for exception route, got: {$boomStatus}");
}
if (strpos($boomResponse, 'Internal Server Error') === false) {
    fail('Expected Internal Server Error body for exception route');
}

$missingStatus = strtok($missingResponse, "\r\n");
if ($missingStatus === false || strpos($missingStatus, '404') === false) {
    fail("Expected 404 for missing route, got: {$missingStatus}");
}

echo "OK\n";
?>
--EXPECT--
OK
