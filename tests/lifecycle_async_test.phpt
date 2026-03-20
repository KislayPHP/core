--TEST--
Kislay\Core\App listenAsync follows the current runtime contract
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
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});

if (defined('PHP_ZTS') && PHP_ZTS) {
    if (!$app->listenAsync($host, $port)) {
        fail('listenAsync failed');
    }
    usleep(150000);
    if (!$app->isRunning()) {
        fail('Expected app to be running after listenAsync');
    }
    if ($app->wait(10) !== false) {
        fail('Expected wait(10) to timeout while server is running');
    }
    $app->stop();
    if ($app->wait(50) !== true) {
        fail('Expected wait(50) to return true after stop()');
    }
} else {
    try {
        @$app->listenAsync($host, $port);
        fail('listenAsync should reject on NTS');
    } catch (Throwable $e) {
        if (strpos($e->getMessage(), 'requires ZTS') === false &&
            strpos($e->getMessage(), 'Failed to start PHP runtime') === false) {
            fail('Unexpected NTS listenAsync message: ' . $e->getMessage());
        }
    }
    if ($app->isRunning()) {
        fail('App should remain stopped after rejected listenAsync');
    }
}

echo "OK\n";
?>
--EXPECT--
OK
