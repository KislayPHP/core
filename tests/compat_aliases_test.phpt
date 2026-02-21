--TEST--
Kislay compatibility helpers: setOption/getJson/isJson and response helpers
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

function make_request($host, $port, $method, $path, $body = '', array $headers = []) {
    $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
    if (!$fp) {
        fail("Failed to connect: {$errstr}");
    }

    $request = "{$method} {$path} HTTP/1.1\r\n";
    $request .= "Host: {$host}:{$port}\r\n";
    foreach ($headers as $name => $value) {
        $request .= "{$name}: {$value}\r\n";
    }
    if ($body !== '') {
        $request .= 'Content-Length: ' . strlen($body) . "\r\n";
    }
    $request .= "Connection: close\r\n\r\n";
    $request .= $body;

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
if ($app->setOption('num_threads', 1) !== true) {
    fail('setOption(num_threads) failed');
}
if ($app->setOption('request_timeout_ms', 5000) !== true) {
    fail('setOption(request_timeout_ms) failed');
}

$app->post('/json', function ($req, $res) {
    $res->json([
        'isJson' => $req->isJson(),
        'payload' => $req->getJson(['fallback' => true]),
    ], 200);
});

$app->get('/nf', function ($req, $res) {
    $res->notFound('missing');
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

$jsonResponse = make_request(
    $host,
    $port,
    'POST',
    '/json',
    '{"x":1}',
    ['Content-Type' => 'application/json']
);
$nfResponse = make_request($host, $port, 'GET', '/nf');

$app->stop();

$jsonStatus = strtok($jsonResponse, "\r\n");
if ($jsonStatus === false || strpos($jsonStatus, '200') === false) {
    fail("Expected 200 for /json, got: {$jsonStatus}");
}
if (strpos($jsonResponse, '"isJson":true') === false) {
    fail('Expected isJson=true in /json response');
}
if (strpos($jsonResponse, '"x":1') === false) {
    fail('Expected payload from getJson in /json response');
}

$nfStatus = strtok($nfResponse, "\r\n");
if ($nfStatus === false || strpos($nfStatus, '404') === false) {
    fail("Expected 404 for /nf, got: {$nfStatus}");
}
if (strpos($nfResponse, 'missing') === false) {
    fail('Expected notFound message body');
}

echo "OK\n";
?>
--EXPECT--
OK
