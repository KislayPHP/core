--TEST--
Kislay Core returns explicit CORS hint for browser preflight when cors is disabled
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

function make_request($host, $port, $method, $path, array $headers = []) {
    $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
    if (!$fp) {
        fail("Failed to connect: {$errstr}");
    }
    $request = "{$method} {$path} HTTP/1.1\r\n";
    $request .= "Host: {$host}:{$port}\r\n";
    foreach ($headers as $name => $value) {
        $request .= "{$name}: {$value}\r\n";
    }
    $request .= "Connection: close\r\n\r\n";
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
$app->get('/api/users', function ($req, $res) {
    $res->json(['ok' => true], 200);
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}
usleep(150000);

$preflight = make_request($host, $port, 'OPTIONS', '/api/users', [
    'Origin' => 'https://frontend.example.com',
    'Access-Control-Request-Method' => 'GET',
]);

$app->stop();

$status = strtok($preflight, "\r\n");
if ($status === false || strpos($status, '403') === false) {
    fail("Expected 403 for CORS-disabled preflight, got: {$status}");
}

if (stripos($preflight, "CORS disabled. Enable with \$app->setOption('cors', true).") === false) {
    fail('Expected explicit CORS disabled hint in response body');
}

echo "OK\n";
?>
--EXPECT--
OK
