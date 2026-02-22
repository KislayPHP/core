--TEST--
Kislay Core falls back to defaults with warnings for invalid configuration values
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
    $request = "{$method} {$path} HTTP/1.1\r\n";
    $request .= "Host: {$host}:{$port}\r\n";
    $request .= "Connection: close\r\n\r\n";
    fwrite($fp, $request);
    $response = stream_get_contents($fp);
    fclose($fp);
    return $response;
}

$warnings = [];
set_error_handler(function ($errno, $errstr) use (&$warnings) {
    $warnings[] = $errstr;
    return true;
});

$app = new Kislay\Core\App();
if ($app->setOption('num_threads', 0) !== true) fail('setOption(num_threads) failed');
if ($app->setOption('request_timeout_ms', -100) !== true) fail('setOption(request_timeout_ms) failed');
if ($app->setOption('max_body', -1) !== true) fail('setOption(max_body) failed');
if ($app->setOption('cors', 'not-bool') !== true) fail('setOption(cors) failed');
if ($app->setOption('referrer_policy', 'bad-policy') !== true) fail('setOption(referrer_policy) failed');
if ($app->setOption('document_root', '/this/path/does/not/exist') !== true) fail('setOption(document_root) failed');
if ($app->setOption('tls_cert', '/tmp/not-existing-cert.pem') !== true) fail('setOption(tls_cert) failed');
if ($app->setOption('tls_key', '/tmp/not-existing-key.pem') !== true) fail('setOption(tls_key) failed');
if ($app->setOption('unknown_option', 'x') !== true) fail('setOption(unknown_option) failed');
if ($app->setMemoryLimit(-1) !== true) fail('setMemoryLimit failed');

$joined = implode("\n", $warnings);
if (strpos($joined, 'invalid num_threads') === false) fail('missing num_threads warning');
if (strpos($joined, 'invalid request_timeout_ms') === false) fail('missing timeout warning');
if (strpos($joined, 'invalid max_body') === false) fail('missing max_body warning');
if (strpos($joined, 'invalid boolean value') === false) fail('missing bool warning');
if (strpos($joined, 'invalid referrer_policy') === false) fail('missing referrer policy warning');
if (strpos($joined, 'document_root=') === false) fail('missing document_root warning');
if (strpos($joined, 'setOption(tls_cert)') === false) fail('missing tls_cert warning');
if (strpos($joined, 'setOption(tls_key)') === false) fail('missing tls_key warning');
if (strpos($joined, 'Unsupported option') === false) fail('missing unknown option warning');
if (strpos($joined, 'setMemoryLimit') === false) fail('missing setMemoryLimit warning');

$app->get('/health', function ($req, $res) {
    $res->json(['ok' => true], 200);
});

$port = reserve_free_port();
if (!$app->listenAsync('127.0.0.1', $port)) {
    fail('listenAsync failed');
}
usleep(150000);
$response = make_request('127.0.0.1', $port, 'GET', '/health');
$app->stop();
restore_error_handler();

$statusLine = strtok($response, "\r\n");
if ($statusLine === false || strpos($statusLine, '200') === false) {
    fail("Expected 200 response, got: {$statusLine}");
}

echo "OK\n";
?>
--EXPECT--
OK
