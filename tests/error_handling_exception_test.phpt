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
require __DIR__ . '/server_helper.inc';

$host = '127.0.0.1';
$port = reserve_free_port();
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->get('/boom', function ($req, $res) {
    throw new Exception('boom');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $boomResponse = make_request($host, $port, 'GET', '/boom');
    $missingResponse = make_request($host, $port, 'GET', '/missing');
} finally {
    stop_kislay_server($server);
}

$boomStatus = strtok($boomResponse, "\r\n");
if ($boomStatus === false || strpos($boomStatus, '500') === false) fail("Expected 500 for exception route, got: {$boomStatus}");
if (strpos($boomResponse, 'Internal Server Error') === false) fail('Expected Internal Server Error body for exception route');
$missingStatus = strtok($missingResponse, "\r\n");
if ($missingStatus === false || strpos($missingStatus, '404') === false) fail("Expected 404 for missing route, got: {$missingStatus}");

echo "OK\n";
?>
--EXPECT--
OK
