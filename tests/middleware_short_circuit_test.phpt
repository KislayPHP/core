--TEST--
Kislay Core middleware can short-circuit without next() chaining
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
    if ($req->path() === '/api/users/blocked') {
        $res->status(401)->send('blocked');
        return false;
    }
    $res->set('X-Chain', '1');
    return true;
});
$app->get('/api/users/:id', function ($req, $res) {
    $res->send('ok:' . $req->param('id'));
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $allowed = make_request($host, $port, 'GET', '/api/users/42');
    $blocked = make_request($host, $port, 'GET', '/api/users/blocked');
} finally {
    stop_kislay_server($server);
}

if (strpos(strtok($allowed, "\r\n"), '200') === false) fail('Expected 200 for allowed request');
if (stripos($allowed, 'x-chain: 1') === false || strpos($allowed, 'ok:42') === false) fail('Allowed middleware/handler chain did not complete');
if (strpos(strtok($blocked, "\r\n"), '401') === false) fail('Expected 401 for blocked request');
if (strpos($blocked, 'blocked') === false) fail('Blocked response body missing');

echo "OK\n";
?>
--EXPECT--
OK
