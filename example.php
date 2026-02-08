<?php

// Run from this folder with:
// php -d extension=modules/kislay_extension.so example.php

// Load the extension (make sure the extension is properly installed and loaded in your PHP configuration)
extension_loaded('kislayphp_extension') or die('The kislayphp_extension is not loaded');

$app = new KislayPHP\Core\App();

// Memory management settings
$app->setMemoryLimit(128 * 1024 * 1024);
$app->enableGc(true);

$app->use(function ($req, $res, $next) {
    // Global middleware
    $req->setAttribute('startTime', microtime(true));
    $next();
});

$app->get('/user/:id', function ($req, $res) {
    $userId = $req->input('id');
    $agent = $req->header('user-agent', 'unknown');
    $res->status(200)
        ->type('text/plain')
        ->send('User ID: ' . $userId . ' Start Time: ' . $req->getAttribute('startTime') . ' UA: ' . $agent);
});

$app->post('/submit', function ($req, $res) {
    $res->json(['all' => $req->all(), 'name' => $req->input('name', 'n/a')], 200);
});

$app->put('/item/:id', function ($req, $res) {
    $res->status(200)->send('Updated item ' . $req->input('id'));
});

$app->patch('/item/:id', function ($req, $res) {
    $res->status(200)->send('Patched item ' . $req->input('id'));
});

$app->delete('/item/:id', function ($req, $res) {
    $res->status(204)->send('');
});

$app->options('/item/:id', function ($req, $res) {
    $res->set('allow', 'GET,POST,PUT,PATCH,DELETE,OPTIONS')->status(204)->send('');
});

$app->group('/api', function ($app) {
    $app->get('/status', function ($req, $res) {
        $res->sendJson(['ok' => true], 200);
    });

    $app->post('/echo', function ($req, $res) {
        $res->sendXml('<echo>' . htmlspecialchars($req->input('msg', '')) . '</echo>', 200);
    });
}, [
    function ($req, $res, $next) {
        $res->header('x-group', 'api');
        $next();
    }
]);

$app->get('/inspect', function ($req, $res) {
    $payload = [
        'method' => $req->method(),
        'path' => $req->path(),
        'query' => $req->getQuery(),
        'queryParams' => $req->getQueryParams(),
        'headers' => $req->getHeaders(),
        'hasToken' => $req->has('token'),
        'only' => $req->only(['token', 'name']),
    ];
    $res->json($payload, 200);
});

// HTTP only
$app->listen('0.0.0.0', 8080);

// HTTPS example (requires civetweb built with OpenSSL)
// $app->listen('0.0.0.0', 8443, ['cert' => '/path/to/cert.pem', 'key' => '/path/to/key.pem']);

?>