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
// Slow enough that the client below can close its socket well before this
// worker finishes and tries to hand the response back to the (by then
// closed) connection.
$app->get('/slow', function ($req, $res) {
    usleep(400000);
    $res->send('slow-ok');
});
$app->get('/ping', function ($req, $res) {
    $res->send('pong');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    // Open a connection, send a request that will be handled slowly, then
    // slam the socket shut (RST via SO_LINGER{onoff=1,linger=0} when the
    // sockets extension is available, otherwise a plain close) WITHOUT
    // reading any response. This mirrors a client resetting mid-request:
    // the libuv loop thread will see nread<0 and close/free bookkeeping for
    // this connection while the request is still executing on the
    // PhpRuntimePool worker thread. Before the fix, the worker would later
    // dereference the freed connection object and crash the whole server
    // process.
    $resetOk = false;
    if (function_exists('socket_create')) {
        $sock = @socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
        if ($sock !== false && @socket_connect($sock, $host, $port)) {
            $req = "GET /slow HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
            socket_write($sock, $req, strlen($req));
            // Force RST on close instead of a graceful FIN.
            socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
            socket_close($sock);
            $resetOk = true;
        } elseif ($sock !== false) {
            socket_close($sock);
        }
    }
    if (!$resetOk) {
        // Fallback: plain abrupt close (still exercises the nread<0 /
        // on_close path in uv_server.cpp, just via EOF instead of RST).
        $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
        if (!$fp) {
            fail("Failed to connect: {$errstr}");
        }
        $req = "GET /slow HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
        fwrite($fp, $req);
        fclose($fp);
    }

    // Give the worker thread time to finish handling /slow (400ms sleep)
    // and attempt to deliver the response to the now-dead connection. This
    // is exactly the window in which the pre-fix code would use freed
    // memory.
    usleep(900000);

    $status = proc_get_status($server['proc']);
    if (!$status['running']) {
        $log = is_file($server['log']) ? trim((string) file_get_contents($server['log'])) : '';
        fail('Server process crashed after client reset during in-flight request' . ($log !== '' ? ": {$log}" : ''));
    }

    // Prove the server is still fully alive and correctly serving new
    // requests on a fresh connection, not just that the process exists.
    $response = make_request($host, $port, 'GET', '/ping');
    $statusLine = strtok($response, "\r\n");
    if ($statusLine === false || strpos($statusLine, '200') === false) {
        fail("Expected 200 from /ping after reset, got: {$statusLine}");
    }
    if (strpos($response, 'pong') === false) {
        fail('Expected pong body after reset');
    }
} finally {
    stop_kislay_server($server);
}

echo "OK\n";
?>
