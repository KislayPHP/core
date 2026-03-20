--TEST--
KislayAsyncHttp handles GET query and JSON request body safely
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
$base = "http://{$host}:{$port}";
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->get('/search', function ($req, $res) {
    $res->json(['q' => $req->query('q')], 200);
});
$app->post('/echo', function ($req, $res) {
    $res->send($req->getBody(), 200);
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $http = new Kislay\Core\AsyncHttp();
    $http->get($base . '/search', ['q' => 'hello world']);
    if (!$http->execute()) fail('GET execute failed');
    if ($http->getResponseCode() !== 200) fail('Expected 200 from GET /search');
    $getBody = $http->getResponse();
    if (strpos($getBody, '"hello world"') === false) fail('GET query parameters were not sent correctly');

    $http->setHeader('Content-Type', 'application/json');
    $http->post($base . '/echo', ['a' => 1, 'b' => 'x']);
    if (!$http->execute()) fail('POST execute failed');
    if ($http->getResponseCode() !== 200) fail('Expected 200 from POST /echo');
    $postBody = $http->getResponse();
    if (strpos($postBody, '"a":1') === false || strpos($postBody, '"b":"x"') === false) fail('POST JSON body was not sent correctly');
} finally {
    stop_kislay_server($server);
}

echo "OK\n";
?>
--EXPECT--
OK
