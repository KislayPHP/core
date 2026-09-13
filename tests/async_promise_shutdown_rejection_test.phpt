--TEST--
Kislay Core rejects still-pending async() promises when the App is destroyed instead of silently dropping their then()/catch()/finally() callbacks
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
if (!defined('PHP_ZTS') || PHP_ZTS == 0) {
    echo 'skip requires ZTS: listenAsync() only returns without blocking under ZTS, which this test needs to leave a task genuinely undrained';
}
?>
--FILE--
<?php
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->listenAsync('127.0.0.1', 18099);

// then()/catch()/finally() each do one opportunistic drain of up to 64
// pending tasks in the promise's lane at registration time (see
// PHP_METHOD(KislayPromise, then) et al.). Queue 100 harmless decoy tasks
// ahead of the real one so that drain exhausts its budget on the decoys
// and never reaches the target task below - leaving it genuinely Pending,
// with a callback already registered, right up until $app is destroyed.
for ($i = 0; $i < 100; $i++) {
    async(function () {
        return 'decoy';
    });
}

$p = async(function () {
    echo "Target task body ran (should never happen)\n";
    return 'unused';
});
$p->then(function ($v) {
    echo "Fulfilled: $v\n";
}, function ($e) {
    echo "Rejected: $e\n";
});

// Deliberately never call $app->wait() - the target task stays queued
// behind the still-undrained decoys. Destroying $app (last reference
// dropped here) must reject its promise and fire the onRejected callback
// above rather than silently discarding it.
unset($app);

echo "Done\n";
?>
--EXPECT--
Rejected: Kislay\Core\App stopped before this task completed
Done
