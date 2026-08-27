--TEST--
Kislay libuv server backend handles real concurrent load correctly: no cross-connection response mixups, and the completion-delivery fix ([[core_libuv_completion_hang]] - RequestCompletion::complete() now virtual) survives concurrent in-flight client resets, not just a single sequential one
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
if (!extension_loaded('curl')) {
    echo 'skip curl extension not loaded';
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
$app->get('/echo', function ($req, $res) {
    $ms = (int) $req->query('ms', '0');
    if ($ms > 0) {
        usleep($ms * 1000);
    }
    $res->send('id:' . $req->query('id'));
});
$app->get('/slow', function ($req, $res) {
    usleep(300000);
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
    // ── Phase 1: real concurrent load, checked for cross-connection mixups ──
    //
    // Under NTS PHP the libuv loop accepts many connections concurrently but
    // PhpRuntimePool::drain() still executes each handler serially on the
    // main thread, then routes the computed response back through
    // UvCompletion::complete() -> enqueue_response() -> uv_async_send() ->
    // process_responses(). The virtual-dispatch fix made that delivery path
    // work at all (see [[core_libuv_completion_hang]]); this phase checks it
    // also delivers each response to the *correct* connection when many are
    // interleaved, not just one at a time. Each request carries a unique id
    // and a small randomized artificial delay so completions genuinely
    // interleave out of submission order, and every response body is
    // checked against the id its own request sent - any mismatch would mean
    // a response got delivered to the wrong socket.
    $concurrency = 30;
    $total = 300;
    $baseUrl = "http://{$host}:{$port}";

    $mh = curl_multi_init();
    $handles = [];
    $expected = [];
    $nextId = 0;

    $addHandle = function () use ($mh, $baseUrl, &$handles, &$expected, &$nextId, $total) {
        if ($nextId >= $total) {
            return false;
        }
        $id = $nextId++;
        $ms = $id % 16; // 0..15ms, deterministic but varied
        $ch = curl_init("{$baseUrl}/echo?id={$id}&ms={$ms}");
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 10);
        $key = (int) $ch;
        $handles[$key] = $ch;
        $expected[$key] = "id:{$id}";
        curl_multi_add_handle($mh, $ch);
        return true;
    };

    for ($i = 0; $i < $concurrency && $i < $total; $i++) {
        $addHandle();
    }

    $completed = 0;
    $mismatches = [];
    $httpErrors = 0;
    $active = null;
    $deadline = microtime(true) + 30.0;

    do {
        while (($status = curl_multi_exec($mh, $active)) === CURLM_CALL_MULTI_PERFORM) {}
        if ($status !== CURLM_OK) {
            break;
        }
        while ($info = curl_multi_info_read($mh)) {
            $ch = $info['handle'];
            $key = (int) $ch;
            $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
            $body = curl_multi_getcontent($ch);
            if ($info['result'] !== CURLE_OK || $code !== 200) {
                $httpErrors++;
            } elseif ($body !== $expected[$key]) {
                $mismatches[] = "expected {$expected[$key]}, got " . var_export($body, true);
            }
            $completed++;
            curl_multi_remove_handle($mh, $ch);
            // curl_close() is a deprecated no-op as of PHP 8.0+; the handle
            // is freed by refcounting once unset() drops the last reference.
            unset($handles[$key], $expected[$key]);
            $addHandle();
        }
        if ($active) {
            curl_multi_select($mh, 0.01);
        }
        if (microtime(true) > $deadline) {
            fail("Phase 1 timed out: {$completed}/{$total} completed, " . count($handles) . " still in flight");
        }
    } while ($active || $handles);

    curl_multi_close($mh);

    if ($httpErrors > 0) {
        fail("Phase 1: {$httpErrors} request(s) failed or returned non-200 out of {$total}");
    }
    if ($completed !== $total) {
        fail("Phase 1: expected {$total} completions, got {$completed}");
    }
    if (!empty($mismatches)) {
        fail('Phase 1: cross-connection response mismatch(es) detected: ' . implode(' | ', array_slice($mismatches, 0, 5))
            . ' (' . count($mismatches) . ' total)');
    }

    // ── Phase 2: concurrent in-flight resets, not just a single one ──
    //
    // tests/uv_server_uaf_test.phpt already proves ONE client resetting
    // mid-request doesn't crash the server. This phase raises that to many
    // simultaneous resets landing on the completion-delivery path in the
    // same short window, which is what the fixed code (virtual
    // RequestCompletion::complete() -> UvCompletion::complete()) actually
    // needs to survive under real concurrent load.
    $rounds = 5;
    $perRound = 16;
    for ($round = 0; $round < $rounds; $round++) {
        $conns = [];
        for ($i = 0; $i < $perRound; $i++) {
            $fp = @fsockopen($host, $port, $errno, $errstr, 2.0);
            if (!$fp) {
                fail("Round {$round}: failed to connect conn {$i}: {$errstr}");
            }
            $req = "GET /slow HTTP/1.1\r\nHost: {$host}:{$port}\r\nConnection: close\r\n\r\n";
            fwrite($fp, $req);
            $conns[$i] = $fp;
        }
        // Let every /slow request actually reach the server and start
        // executing before we start tearing connections down, so the resets
        // land while handlers are genuinely in flight (matching the
        // uaf_test's own timing rationale), not before the server even read
        // them.
        usleep(50000);
        foreach ($conns as $i => $fp) {
            if ($i % 2 === 0) {
                // Abrupt close without reading: exercises the concurrent
                // nread<0/on_close + late-completion-delivery race.
                fclose($fp);
            } else {
                // Let the other half complete normally, interleaved with the
                // resets above, and verify they still get the right answer.
                $response = stream_get_contents($fp);
                fclose($fp);
                if (strpos($response, 'slow-ok') === false) {
                    fail("Round {$round}: conn {$i} expected slow-ok, got: " . substr($response, 0, 200));
                }
            }
        }
        // Give the worker thread time to finish the reset connections'
        // handlers (300ms sleep each) and attempt delivery to the now-dead
        // sockets - the exact window the original single-connection UAF
        // bug lived in, now with 8 simultaneous late deliveries per round.
        usleep(500000);

        $status = proc_get_status($server['proc']);
        if (!$status['running']) {
            $log = is_file($server['log']) ? trim((string) file_get_contents($server['log'])) : '';
            fail("Server process crashed during concurrent-reset round {$round}" . ($log !== '' ? ": {$log}" : ''));
        }
    }

    // ── Final liveness + correctness check ──
    $response = make_request($host, $port, 'GET', '/ping');
    $statusLine = strtok($response, "\r\n");
    if ($statusLine === false || strpos($statusLine, '200') === false) {
        fail("Expected 200 from /ping after concurrency stress, got: {$statusLine}");
    }
    if (strpos($response, 'pong') === false) {
        fail('Expected pong body after concurrency stress');
    }
} finally {
    stop_kislay_server($server);
}

echo "OK\n";
?>
--EXPECT--
OK
