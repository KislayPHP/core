--TEST--
Kislay Core only applies compiled path middleware to matched routes
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
$app->use('/api', function ($req, $res) {
    $res->set('X-Powered-By', 'Kislay');
    return true;
});
$app->get('/api/users', function ($req, $res) {
    $res->json(['ok' => true], 200);
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $response = make_request($host, $port, 'GET', '/api/site/home');
} finally {
    stop_kislay_server($server);
}

$status = strtok($response, "\r\n");
if ($status === false || strpos($status, '404') === false) fail("Expected 404 for unmatched route, got: {$status}");
if (stripos($response, 'x-powered-by: Kislay') !== false) fail('Unmatched route should not inherit compiled middleware headers');
if (strpos($response, 'Not Found') === false) fail('Expected Not Found response body');

echo "OK\n";
?>
--EXPECT--
OK
