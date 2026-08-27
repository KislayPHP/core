--TEST--
Kislay libuv server backend registers the active app so async()/AsyncHttp work (previously kislay_active_app was only set by the CivetWeb start path, so every server_type=libuv app under listen()/listenAsync() had it stuck at nullptr)
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
$app->setOption('server_type', 'libuv');
// Before the fix, kislay_app_start_server_uv() never called
// kislay_active_app.store(app, ...) (only the CivetWeb-specific
// kislay_app_start_server() did), so the global async() function and
// KislayAsyncHttp::execute()/executeAsync() - both of which read
// kislay_active_app to find "the current app" - always threw "No active
// Kislay App found for async operation" on a server_type=libuv app, even
// though the exact same route worked fine on the default CivetWeb backend.
$app->get('/async-fire', function ($req, $res) {
    async(function () {
        return 42;
    });
    $res->send('fired');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $response = make_request($host, $port, 'GET', '/async-fire');
    $statusLine = strtok($response, "\r\n");
    if ($statusLine === false || strpos($statusLine, '200') === false) {
        fail("Expected 200 from /async-fire on libuv, got: {$statusLine}");
    }
    if (strpos($response, 'fired') === false) {
        fail('Expected fired body from /async-fire on libuv, got: ' . $response);
    }
} finally {
    stop_kislay_server($server);
}

echo "OK\n";
?>
--EXPECT--
OK
