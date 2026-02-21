--TEST--
Kislay Core normalizes trailing slashes for route matching
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

function make_request($host, $port, $path) {
    $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
    if (!$fp) {
        fail("Failed to connect: {$errstr}");
    }

    $request = "GET {$path} HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
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
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

$withoutSlash = make_request($host, $port, '/health');
$withSlash = make_request($host, $port, '/health/');

$app->stop();

$statusA = strtok($withoutSlash, "\r\n");
if ($statusA === false || strpos($statusA, '200') === false) {
    fail("Expected 200 for /health, got: {$statusA}");
}

$statusB = strtok($withSlash, "\r\n");
if ($statusB === false || strpos($statusB, '200') === false) {
    fail("Expected 200 for /health/, got: {$statusB}");
}

if (strpos($withSlash, 'ok') === false) {
    fail('Expected body for /health/');
}

echo "OK\n";
?>
--EXPECT--
OK
