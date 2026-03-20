--TEST--
Kislay\Core\App listenAsync rejects invalid lifecycle transitions
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
        fail('first listenAsync failed');
    }
    usleep(150000);
    try {
        $app->listenAsync($host, $port);
        $app->stop();
        fail('second listenAsync should throw');
    } catch (Throwable $e) {
        if (strpos($e->getMessage(), 'Server already running') === false) {
            $app->stop();
            fail('unexpected exception message: ' . $e->getMessage());
        }
    }
    $app->stop();
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
}

echo "OK\n";
?>
--EXPECT--
OK
