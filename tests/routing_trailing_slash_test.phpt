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
require __DIR__ . '/server_helper.inc';

$host = '127.0.0.1';
$port = reserve_free_port();
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $withoutSlash = make_request($host, $port, 'GET', '/health');
    $withSlash = make_request($host, $port, 'GET', '/health/');
} finally {
    stop_kislay_server($server);
}

$statusA = strtok($withoutSlash, "\r\n");
if ($statusA === false || strpos($statusA, '200') === false) fail("Expected 200 for /health, got: {$statusA}");
$statusB = strtok($withSlash, "\r\n");
if ($statusB === false || strpos($statusB, '200') === false) fail("Expected 200 for /health/, got: {$statusB}");
if (strpos($withSlash, 'ok') === false) fail('Expected body for /health/');

echo "OK\n";
?>
--EXPECT--
OK
