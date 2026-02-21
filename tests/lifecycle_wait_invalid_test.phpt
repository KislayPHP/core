--TEST--
Kislay\Core\App wait rejects invalid timeout
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
?>
--FILE--
<?php
$app = new Kislay\Core\App();

try {
    $app->wait(-2);
    echo "FAIL: expected exception\n";
    exit(1);
} catch (Throwable $e) {
    if (strpos($e->getMessage(), 'timeoutMs must be >= -1') === false) {
        echo 'FAIL: unexpected exception message: ' . $e->getMessage() . "\n";
        exit(1);
    }
}

echo "OK\n";
?>
--EXPECT--
OK
