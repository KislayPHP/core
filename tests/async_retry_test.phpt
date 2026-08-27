--TEST--
KislayAsyncHttp retry and correlation-id propagation
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
if (defined('PHP_ZTS') && PHP_ZTS == 0) {
    echo 'skip async retry test requires ZTS build for deterministic thread behavior';
}
?>
--FILE--
<?php
$app = new Kislay\Core\App();
$app->setOption('log', false);
$port = 9200;

$requestCount = 0;
$receivedCorrelationId = null;

$app->get('/retry-test', function($req, $res) use (&$requestCount, &$receivedCorrelationId) {
    $requestCount++;
    $receivedCorrelationId = $req->header('x-correlation-id');
    
    if ($requestCount === 1) {
        // Fail the first time to trigger retry. Must be >=500: the retry
        // worker (worker_pool.cpp's execute_and_maybe_retry) only treats a
        // transport failure or 5xx as retryable by design - retrying an
        // identical 4xx would just get the same client error again. A 404
        // here was a bug in this test's own premise, not the runtime: it
        // never actually exercised the retry path at all (silently, since
        // nothing before this checked $requestCount reached 2).
        $res->status(503)->send("Failed first");
    } else {
        $res->status(200)->send("Success on try {$requestCount}");
    }
});

$app->listenAsync('127.0.0.1', $port);

// We need an active request context to test auto-propagation.
// Since we are in CLI, we can't easily simulate a "current" request without 
// actually being inside a Kislay route handler.
// But we can test if it generates a NEW one if missing.

$http = new Kislay\Core\AsyncHttp();
$http->get("http://127.0.0.1:{$port}/retry-test");
$http->retry(2, 100); // 2 retries, 100ms delay

$http->executeAsync()->then(function($res) use ($http, &$requestCount, &$receivedCorrelationId) {
    echo "Final Status: " . $http->getResponseCode() . "
";
    echo "Total Requests: " . $requestCount . "
";
    echo "Has Correlation ID: " . ($receivedCorrelationId ? 'yes' : 'no') . "
";
})->catch(function($err) {
    echo "Error: " . $err . "
";
});

// Wait for the async tasks to complete. Must pump via $app->wait() (which
// drains the async bridge - see kislay_app_wait_loop()/kislay_async_drain()
// in kislay_extension.cpp), not a plain usleep() loop: nothing else resolves
// the executeAsync() promise or fires its then()/catch() callbacks, so a
// bare sleep loop here waits out the full timeout with zero output, whether
// or not the underlying HTTP retries actually completed server-side.
// Since we have a 100ms delay and 2 tries, 500ms should be enough.
$start = microtime(true);
while (microtime(true) - $start < 1.0) {
    if ($requestCount >= 2) break;
    $app->wait(10);
}

$app->stop();
?>
--EXPECTF--
Final Status: 200
Total Requests: 2
Has Correlation ID: yes
