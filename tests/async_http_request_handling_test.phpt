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

$host = '127.0.0.1';
$port = reserve_free_port();
$base = "http://{$host}:{$port}";

$app = new Kislay\Core\App();
$app->get('/search', function ($req, $res) {
    $res->json(['q' => $req->query('q')], 200);
});
$app->post('/echo', function ($req, $res) {
    $res->send($req->getBody(), 200);
});

if (!$app->listenAsync($host, $port)) {
    fail('listenAsync failed');
}

usleep(150000);

$http = new Kislay\Core\AsyncHttp();
$http->get($base . '/search', ['q' => 'hello world']);
if (!$http->execute()) {
    $app->stop();
    fail('GET execute failed');
}
if ($http->getResponseCode() !== 200) {
    $app->stop();
    fail('Expected 200 from GET /search');
}
$getBody = $http->getResponse();
if (strpos($getBody, '"hello world"') === false) {
    $app->stop();
    fail('GET query parameters were not sent correctly');
}

$http->setHeader('Content-Type', 'application/json');
$http->post($base . '/echo', ['a' => 1, 'b' => 'x']);
if (!$http->execute()) {
    $app->stop();
    fail('POST execute failed');
}
if ($http->getResponseCode() !== 200) {
    $app->stop();
    fail('Expected 200 from POST /echo');
}
$postBody = $http->getResponse();
if (strpos($postBody, '"a":1') === false || strpos($postBody, '"b":"x"') === false) {
    $app->stop();
    fail('POST JSON body was not sent correctly');
}

$app->stop();
echo "OK\n";
?>
--EXPECT--
OK
