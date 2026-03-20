--TEST--
Kislay Core Promise-based async features (async, then, catch, finally, executeAsync)
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
if (!defined('PHP_ZTS') || PHP_ZTS == 0) {
    echo 'skip promise async test requires ZTS listenAsync support';
}
?>
--FILE--
<?php
$app = new Kislay\Core\App();
$app->setOption('async', true);
$app->setOption('log', false);
$app->get('/hello', function($req, $res) {
    $res->send('Hello World');
});
$app->listenAsync('127.0.0.1', 8082);

$p1 = async(function() {
    return 'Result 1';
});
$p1->then(function($val) {
    echo "Fulfillment: $val\n";
});
$p1->finally(function() {
    echo "Settled 1\n";
});

$p2 = async(function() {
    throw new Exception('Error 2');
});
$p2->catch(function($err) {
    echo 'Rejection: ' . $err->getMessage() . "\n";
});
$p2->finally(function() {
    echo "Settled 2\n";
});

$http = new Kislay\Core\AsyncHttp();
$http->get('http://127.0.0.1:8082/hello');
$p3 = $http->executeAsync();
$p3->then(function($res) use ($http) {
    echo 'HTTP Status: ' . $http->getResponseCode() . "\n";
    echo 'HTTP Body: ' . $http->getResponse() . "\n";
});
$p3->finally(function() {
    echo "Settled 3\n";
});

$app->wait(1000);
$app->stop();
?>
--EXPECTF--
Fulfillment: Result 1
Settled 1
Rejection: Error 2
Settled 2
HTTP Status: 200
HTTP Body: Hello World
Settled 3
