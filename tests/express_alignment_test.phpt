--TEST--
KislayPHP Core Express-style all/use(path)/sendStatus
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

$app = new KislayPHP\Core\App();
$app->use('/api', function ($req, $res, $next) {
    $res->set('X-Scoped', '1');
    $next();
});

$app->all('/api/health', function ($req, $res) {
    $res->sendStatus(204);
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

$getResponse = make_request($host, $port, 'GET', '/api/health');
$postResponse = make_request($host, $port, 'POST', '/api/health');

$app->stop();

$getFirstLine = strtok($getResponse, "\r\n");
if ($getFirstLine === false || strpos($getFirstLine, '204') === false) {
    fail("GET expected 204, got: {$getFirstLine}");
}

$postFirstLine = strtok($postResponse, "\r\n");
if ($postFirstLine === false || strpos($postFirstLine, '204') === false) {
    fail("POST expected 204, got: {$postFirstLine}");
}

if (stripos($getResponse, "x-scoped: 1") === false) {
    fail('Scoped middleware header missing for GET');
}

if (stripos($postResponse, "x-scoped: 1") === false) {
    fail('Scoped middleware header missing for POST');
}

echo "OK\n";
?>
--EXPECT--
OK
