--TEST--
Kislay libuv server backend streams the actual file body for Response::sendFile() (previously UvServer::process_responses() had no idea about send_file/file_path at all, so it emitted a duplicate Content-Length header and an empty body)
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
$fixture = tempnam(sys_get_temp_dir(), 'kislay_sendfile_');
$fixtureBody = str_repeat('K', 4096);
file_put_contents($fixture, $fixtureBody);

$bootstrap = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('server_type', 'libuv');
$app->get('/dl', function ($req, $res) {
    $res->sendFile('__FIXTURE__');
});
$app->listen($host, $port);
PHP;
$bootstrap = strtr($bootstrap, [
    '__HOST__' => $host,
    '__PORT__' => (string) $port,
    '__FIXTURE__' => $fixture,
]);
$server = start_kislay_server($bootstrap, $host, $port);

try {
    $response = make_request($host, $port, 'GET', '/dl');
    $statusLine = strtok($response, "\r\n");
    if ($statusLine === false || strpos($statusLine, '200') === false) {
        fail("Expected 200 from /dl, got: {$statusLine}");
    }

    $parts = explode("\r\n\r\n", $response, 2);
    if (count($parts) !== 2) {
        fail('Malformed response, missing header/body separator: ' . $response);
    }
    [$headerBlock, $body] = $parts;

    // Before the fix this matched twice - once from Response::sendFile()'s
    // own manual "content-length" header entry (left in res->headers,
    // never stripped for the libuv path) and once from
    // process_responses()'s own unconditional "Content-Length: " .
    // body.size() line (0, since sendFile() clears body and nothing ever
    // populated it for libuv).
    $contentLengthCount = preg_match_all('/^content-length:/im', $headerBlock);
    if ($contentLengthCount !== 1) {
        fail("Expected exactly one Content-Length header, found {$contentLengthCount}: {$headerBlock}");
    }

    if ($body !== $fixtureBody) {
        fail('Expected the actual file body (4096 bytes of K), got ' . strlen($body) . ' bytes: ' . substr($body, 0, 100));
    }
} finally {
    stop_kislay_server($server);
    @unlink($fixture);
}

echo "OK\n";
?>
--EXPECT--
OK
