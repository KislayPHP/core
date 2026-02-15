--TEST--
KislayPHP\Core\App async lifecycle methods
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

    if (!$name || strpos($name, ':') === false) {
        fail('Failed to determine reserved port');
    }

    $parts = explode(':', $name);
    $port = (int) end($parts);
    if ($port <= 0) {
        fail('Reserved port is invalid');
    }

    return $port;
}

$host = '127.0.0.1';
$port = reserve_free_port();

$app = new KislayPHP\Core\App();
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

if (!$app->isRunning()) {
    fail('Expected app to be running after listenAsync');
}

if ($app->wait(10) !== false) {
    fail('Expected wait(10) to timeout while server is running');
}

$fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
if (!$fp) {
    $app->stop();
    fail("Failed to connect to async server: {$errstr}");
}

$request = "GET /health HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
fwrite($fp, $request);
$response = stream_get_contents($fp);
fclose($fp);

if ($response === false || $response === '') {
    $app->stop();
    fail('Empty response from async server');
}

$firstLine = strtok($response, "\r\n");
if ($firstLine === false || strpos($firstLine, '200') === false) {
    $app->stop();
    fail("Unexpected status line: {$firstLine}");
}

$app->stop();

if ($app->isRunning()) {
    fail('Expected app to be stopped after stop()');
}

if ($app->wait(50) !== true) {
    fail('Expected wait(50) to return true after stop()');
}

if ($app->stop() !== true) {
    fail('Expected stop() to be idempotent and return true');
}

echo "OK\n";
?>
--EXPECT--
OK
