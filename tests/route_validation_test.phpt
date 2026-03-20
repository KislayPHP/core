--TEST--
Kislay Core rejects unsupported regex-style route patterns
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

$app = new Kislay\Core\App();
$app->setOption('log', false);

try {
    $app->get('/users/(.*)', function ($req, $res) {});
    fail('Expected regex route to throw');
} catch (Throwable $e) {
    if (strpos($e->getMessage(), 'Regex routes are no longer supported') === false) {
        fail('Unexpected regex route message: ' . $e->getMessage());
    }
}

try {
    $app->get('/users/:bad-name', function ($req, $res) {});
    fail('Expected invalid param route to throw');
} catch (Throwable $e) {
    if (strpos($e->getMessage(), 'Invalid route parameter name') === false) {
        fail('Unexpected invalid param route message: ' . $e->getMessage());
    }
}

echo "OK\n";
?>
--EXPECT--
OK
