--TEST--
Kislay Core uses strict segment routes and precompiled scoped middleware
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
    $res->set('X-Scoped', '1');
    return true;
});
$app->get('/api/users/:id', function ($req, $res) {
    $res->json(['id' => $req->param('id'), 'q' => $req->query('q', '')], 200);
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $getResponse = make_request($host, $port, 'GET', '/api/users/42?q=neo');
    $missingResponse = make_request($host, $port, 'GET', '/api/users/42/profile');
} finally {
    stop_kislay_server($server);
}

$getFirstLine = strtok($getResponse, "\r\n");
if ($getFirstLine === false || strpos($getFirstLine, '200') === false) fail("GET expected 200, got: {$getFirstLine}");
if (strpos($getResponse, '"id":"42"') === false || strpos($getResponse, '"q":"neo"') === false) fail('GET param/query payload missing');
if (stripos($getResponse, 'x-scoped: 1') === false) fail('Scoped middleware header missing');
$missingFirstLine = strtok($missingResponse, "\r\n");
if ($missingFirstLine === false || strpos($missingFirstLine, '404') === false) fail("Strict segment matching failed, got: {$missingFirstLine}");

echo "OK\n";
?>
--EXPECT--
OK
