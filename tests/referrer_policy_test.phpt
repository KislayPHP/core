--TEST--
Kislay Core applies configurable Referrer-Policy headers
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

$portDefault = reserve_free_port();
$bootstrapDefault = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('cors', true);
$app->get('/health', function ($req, $res) {
    $res->json(['ok' => true], 200);
});
$app->listen($host, $port);
PHP;
$bootstrapDefault = strtr($bootstrapDefault, ['__HOST__' => $host, '__PORT__' => (string) $portDefault]);
$serverDefault = start_kislay_server($bootstrapDefault, $host, $portDefault);
try {
    $defaultGet = make_request($host, $portDefault, 'GET', '/health');
    $defaultOptions = make_request($host, $portDefault, 'OPTIONS', '/health', '', [
        'Origin' => 'https://example.com',
        'Access-Control-Request-Method' => 'GET',
    ]);
} finally {
    stop_kislay_server($serverDefault);
}
if (stripos($defaultGet, 'referrer-policy: strict-origin-when-cross-origin') === false) fail('default referrer policy header missing');
if (stripos($defaultOptions, 'referrer-policy: strict-origin-when-cross-origin') === false) fail('default referrer policy header missing on OPTIONS');

$portCustom = reserve_free_port();
$bootstrapCustom = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('referrer_policy', 'origin-when-cross-origin');
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});
$app->listen($host, $port);
PHP;
$bootstrapCustom = strtr($bootstrapCustom, ['__HOST__' => $host, '__PORT__' => (string) $portCustom]);
$serverCustom = start_kislay_server($bootstrapCustom, $host, $portCustom);
try {
    $customGet = make_request($host, $portCustom, 'GET', '/health');
} finally {
    stop_kislay_server($serverCustom);
}
if (stripos($customGet, 'referrer-policy: origin-when-cross-origin') === false) fail('custom referrer policy header missing');

$portDisabled = reserve_free_port();
$bootstrapDisabled = <<<'PHP'
$host = '__HOST__';
$port = __PORT__;
$app = new Kislay\Core\App();
$app->setOption('log', false);
$app->setOption('referrer_policy', 'off');
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});
$app->listen($host, $port);
PHP;
$bootstrapDisabled = strtr($bootstrapDisabled, ['__HOST__' => $host, '__PORT__' => (string) $portDisabled]);
$serverDisabled = start_kislay_server($bootstrapDisabled, $host, $portDisabled);
try {
    $disabledGet = make_request($host, $portDisabled, 'GET', '/health');
} finally {
    stop_kislay_server($serverDisabled);
}
if (stripos($disabledGet, 'referrer-policy:') !== false) fail('referrer policy header should be disabled');

echo "OK\n";
?>
--EXPECT--
OK
