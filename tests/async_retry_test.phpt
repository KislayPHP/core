--TEST--
KislayAsyncHttp retry and correlation-id propagation
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
?>
--FILE--
<?php
$app = new Kislay\Core\App();
$port = 9200;

$requestCount = 0;
$receivedCorrelationId = null;

$app->get('/retry-test', function($req, $res) use (&$requestCount, &$receivedCorrelationId) {
    $requestCount++;
    $receivedCorrelationId = $req->header('x-correlation-id');
    
    if ($requestCount === 1) {
        // Fail the first time to trigger retry
        $res->status(404)->send("Failed first");
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
    
    $app = Kislay\Core\App::isRunning() ? null : null; // just to keep script alive if needed, but wait() is better
})->catch(function($err) {
    echo "Error: " . $err . "
";
});

// Wait for the async tasks to complete
// Since we have a 100ms delay and 2 tries, 500ms should be enough.
$start = microtime(true);
while (microtime(true) - $start < 1.0) {
    if ($requestCount >= 2) break;
    usleep(10000);
}

$app->stop();
?>
--EXPECTF--
Final Status: 200
Total Requests: 2
Has Correlation ID: yes
