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
require __DIR__ . '/server_helper.inc';

$host = '127.0.0.1';
$port = reserve_free_port();
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->get('/api/users', function ($req, $res) {
    $res->json(['ok' => true], 200);
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $preflight = make_request($host, $port, 'OPTIONS', '/api/users', '', [
        'Origin' => 'https://frontend.example.com',
        'Access-Control-Request-Method' => 'GET',
    ]);
} finally {
    stop_kislay_server($server);
}

$status = strtok($preflight, "\r\n");
if ($status === false || strpos($status, '403') === false) fail("Expected 403 for CORS-disabled preflight, got: {$status}");
if (stripos($preflight, "CORS disabled. Enable with \$app->setOption('cors', true).") === false) fail('Expected explicit CORS disabled hint in response body');

echo "OK\n";
?>
--EXPECT--
OK
