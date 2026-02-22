--TEST--
Kislay Core applies configurable Referrer-Policy headers
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

$appDefault = new Kislay\Core\App();
if ($appDefault->setOption('cors', true) !== true) {
    fail('setOption(cors) failed');
}
$appDefault->get('/health', function ($req, $res) {
    $res->json(['ok' => true], 200);
});
$portDefault = reserve_free_port();
if (!$appDefault->listenAsync($host, $portDefault)) {
    fail('listenAsync default app failed');
}
usleep(150000);
$defaultGet = make_request($host, $portDefault, 'GET', '/health');
$defaultOptions = make_request($host, $portDefault, 'OPTIONS', '/health', [
    'Origin' => 'https://example.com',
    'Access-Control-Request-Method' => 'GET',
]);
$appDefault->stop();

if (stripos($defaultGet, "referrer-policy: strict-origin-when-cross-origin") === false) {
    fail('default referrer policy header missing');
}
if (stripos($defaultOptions, "referrer-policy: strict-origin-when-cross-origin") === false) {
    fail('default referrer policy header missing on OPTIONS');
}

$appCustom = new Kislay\Core\App();
if ($appCustom->setOption('referrer_policy', 'origin-when-cross-origin') !== true) {
    fail('setOption(referrer_policy) failed');
}
$appCustom->get('/health', function ($req, $res) {
    $res->send('ok');
});
$portCustom = reserve_free_port();
if (!$appCustom->listenAsync($host, $portCustom)) {
    fail('listenAsync custom app failed');
}
usleep(150000);
$customGet = make_request($host, $portCustom, 'GET', '/health');
$appCustom->stop();
if (stripos($customGet, "referrer-policy: origin-when-cross-origin") === false) {
    fail('custom referrer policy header missing');
}

$appDisabled = new Kislay\Core\App();
if ($appDisabled->setOption('referrer_policy', 'off') !== true) {
    fail('setOption(referrer_policy off) failed');
}
$appDisabled->get('/health', function ($req, $res) {
    $res->send('ok');
});
$portDisabled = reserve_free_port();
if (!$appDisabled->listenAsync($host, $portDisabled)) {
    fail('listenAsync disabled app failed');
}
usleep(150000);
$disabledGet = make_request($host, $portDisabled, 'GET', '/health');
$appDisabled->stop();
if (stripos($disabledGet, "referrer-policy:") !== false) {
    fail('referrer policy header should be disabled');
}

echo "OK\n";
?>
--EXPECT--
OK
