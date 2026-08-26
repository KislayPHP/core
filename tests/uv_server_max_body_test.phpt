--TEST--
Kislay libuv server backend enforces max_body_bytes (previously unbounded, unlike the CivetWeb backend)
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
?>
--FILE--
<?php
require __DIR__ . '/server_helper.inc';

// NOTE: this test deliberately avoids make_request()/a positive "under the
// cap succeeds" assertion. Both would round-trip through
// PhpRuntimePool::drain() -> the request handler -> completion ->
// enqueue_response() on the libuv backend, and that path currently hangs
// indefinitely under NTS PHP in this environment (reproduced independently
// of this change - the pre-existing tests/uv_server_uaf_test.phpt's final
// make_request('/ping') hits the exact same hang on an unmodified checkout).
// That is a separate, more severe, NOT-yet-understood bug in
// PhpRuntimePool/UvServer request completion, out of scope for this fix -
// see the project memory/report for details. This test instead verifies
// only what max_body_bytes's fix actually touches: UvServer::on_body's cap
// check and on_read's now-honored llhttp error return, neither of which go
// through drain()/the completion path at all (the connection is closed
// directly from the libuv I/O callback), so this test is unaffected by that
// separate bug and gives a real, hang-proof signal for THIS fix.
$host = '127.0.0.1';
$port = reserve_free_port();
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('server_type', 'libuv');
$app->setOption('max_body_bytes', 1024);
$app->post('/upload', function ($req, $res) {
    $res->send('accepted:' . strlen($req->getBody()));
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, ['__HOST__' => $host, '__PORT__' => (string) $port]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    // Over the cap (configured to 1024 bytes above): before the fix,
    // UvServer::on_body appended to the request body with no limit at all
    // (only each individual libuv read chunk was capped at 64KB, which does
    // nothing to bound the total accumulated across many reads/chunks) - a
    // client could stream an arbitrarily large body and grow server memory
    // without limit. The fix makes on_body abort parsing once the cap is
    // exceeded, and on_read close the connection instead of silently
    // discarding llhttp's error return and leaving it open forever.
    $big = str_repeat('b', 4096);
    $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
    if (!$fp) {
        fail("Failed to connect: {$errstr}");
    }
    stream_set_timeout($fp, 5);
    $request = "POST /upload HTTP/1.1\r\nHost: {$host}:{$port}\r\n";
    $request .= 'Content-Length: ' . strlen($big) . "\r\n";
    $request .= "Connection: close\r\n\r\n";
    $request .= $big;
    fwrite($fp, $request);
    $oversizedResponse = stream_get_contents($fp);
    $meta = stream_get_meta_data($fp);
    fclose($fp);

    // The connection must be aborted (empty response) rather than the
    // request handler ever running (which would need the still-hanging
    // completion path to answer "accepted:4096" - if that somehow appeared
    // it would mean the cap did not stop the oversized body from reaching
    // the handler at all).
    if (strpos($oversizedResponse, 'accepted:4096') !== false) {
        fail('Server accepted a body over max_body_bytes instead of rejecting/aborting it: ' . $oversizedResponse);
    }
    if ($meta['timed_out']) {
        fail('Connection was left open instead of being closed after the oversized body (on_read is not honoring the on_body error return)');
    }
    if ($oversizedResponse !== '') {
        fail('Expected the connection to be closed with no response body, got: ' . $oversizedResponse);
    }

    // The server process itself must still be alive - a bug here shouldn't
    // take down the whole process. (Not exercising a second real
    // request/response here, for the reason explained above.)
    usleep(200000);
    $status = proc_get_status($server['proc']);
    if (!$status['running']) {
        $log = is_file($server['log']) ? trim((string) file_get_contents($server['log'])) : '';
        fail('Server process crashed after an oversized body' . ($log !== '' ? ": {$log}" : ''));
    }
} finally {
    stop_kislay_server($server);
}

echo "OK\n";
?>
--EXPECT--
OK
