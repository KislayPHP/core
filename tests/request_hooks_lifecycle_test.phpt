--TEST--
Kislay Core executes onRequestStart and onRequestEnd hooks for each request
--SKIPIF--
<?php
if (!extension_loaded('kislayphp_extension')) {
    echo 'skip kislayphp_extension not loaded';
}
?>
--FILE--
<?php
require __DIR__ . '/server_helper.inc';

$host = '127.0.0.1';
$port = reserve_free_port();
$countFile = tempnam(sys_get_temp_dir(), 'kislay_hooks_');
$countFileLiteral = var_export($countFile, true);
file_put_contents($countFile, json_encode(['start' => 0, 'end' => 0]));
$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$countFile = __COUNT_FILE__;
$writeCounts = function ($start, $end) use ($countFile) {
    file_put_contents($countFile, json_encode(['start' => $start, 'end' => $end]), LOCK_EX);
};
$startCount = 0;
$endCount = 0;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->onRequestStart(function ($req, $res) use (&$startCount, &$endCount, $writeCounts) {
    $startCount++;
    $writeCounts($startCount, $endCount);
});
$app->onRequestEnd(function ($req, $res) use (&$startCount, &$endCount, $writeCounts) {
    $endCount++;
    $writeCounts($startCount, $endCount);
});
$app->get('/health', function ($req, $res) {
    $res->json(['ok' => true], 200);
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, [
    '__HOST__' => $host,
    '__PORT__' => (string) $port,
    '__COUNT_FILE__' => $countFileLiteral,
]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $response = make_request($host, $port, 'GET', '/health');
    usleep(200000);
} finally {
    stop_kislay_server($server);
}

$status = strtok($response, "\r\n");
if ($status === false || strpos($status, '200') === false) {
    @unlink($countFile);
    fail("Expected 200 response, got: {$status}");
}
$counts = json_decode((string) file_get_contents($countFile), true);
@unlink($countFile);
if (!is_array($counts) || ($counts['start'] ?? null) !== 1) fail('Expected onRequestStart count=1');
if (($counts['end'] ?? null) !== 1) fail('Expected onRequestEnd count=1');

echo "OK\n";
?>
--EXPECT--
OK
