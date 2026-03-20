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
require __DIR__ . '/server_helper.inc';

$host = '127.0.0.1';
$port = reserve_free_port();
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('num_threads', 1);
$app->setOption('request_timeout_ms', 5000);
$app->post('/json', function ($req, $res) {
    $res->json([
        'isJson' => $req->isJson(),
        'payload' => $req->getJson(['fallback' => true]),
    ], 200);
});
$app->get('/nf', function ($req, $res) {
    $res->notFound('missing');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $jsonResponse = make_request($host, $port, 'POST', '/json', '{"x":1}', ['Content-Type' => 'application/json']);
    $nfResponse = make_request($host, $port, 'GET', '/nf');
} finally {
    stop_kislay_server($server);
}

$jsonStatus = strtok($jsonResponse, "\r\n");
if ($jsonStatus === false || strpos($jsonStatus, '200') === false) fail("Expected 200 for /json, got: {$jsonStatus}");
if (strpos($jsonResponse, '"isJson":true') === false) fail('Expected isJson=true in /json response');
if (strpos($jsonResponse, '"x":1') === false) fail('Expected payload from getJson in /json response');
$nfStatus = strtok($nfResponse, "\r\n");
if ($nfStatus === false || strpos($nfStatus, '404') === false) fail("Expected 404 for /nf, got: {$nfStatus}");
if (strpos($nfResponse, 'missing') === false) fail('Expected notFound message body');

echo "OK\n";
?>
--EXPECT--
OK
