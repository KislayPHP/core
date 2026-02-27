<?php
// KislayPHP Core minimal test server
// Run with: php -d extension=modules/kislay_extension.so server.php

if (!extension_loaded('kislayphp_extension')) {
    die('The kislayphp_extension is not loaded');
}

$app = new Kislay\Core\App();

// Optional: Set memory and GC if supported
if (method_exists($app, 'setMemoryLimit')) {
    $app->setMemoryLimit(128 * 1024 * 1024);
}
if (method_exists($app, 'enableGc')) {
    $app->enableGc(true);
}

// Root route
$app->get('/', function($req, $res) {
    $res->send('Hello from KislayPHP!');
});

// Simple test route
$app->get('/ping', function($req, $res) {
    $res->json(['pong' => true], 200);
});

// Echo POST body
$app->post('/echo', function($req, $res) {
    $res->json(['body' => $req->all()], 200);
});

// Start server
$app->listen('0.0.0.0', 9008);
