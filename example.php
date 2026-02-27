<?php

// Run from this folder with:
// php -d extension=modules/kislay_extension.so example.php

// Load the extension (make sure the extension is properly installed and loaded in your PHP configuration)
extension_loaded('kislayphp_extension') or die('The kislayphp_extension is not loaded');

$app = new Kislay\Core\App();

// 1. Enable asynchronous event loop (required for async() and executeAsync)
$app->setOption('async', true);

// Memory management settings
$app->setMemoryLimit(128 * 1024 * 1024);
$app->enableGc(true);

$app->use(function ($req, $res, $next) {
    // Global middleware
    echo "Incoming request: " . $req->method() . " " . $req->path() . "\n";
    $req->setAttribute('startTime', microtime(true));
    $next();
});

// Example 2: Promise-based Async Task
$app->get('/async-calc', function ($req, $res) {
    echo "Starting background calculation...\n";
    
    // Offload work to the background event loop
    async(function() {
        // Simulate a heavy task
        $sum = 0;
        for($i=0; $i<1000000; $i++) $sum += $i;
        return $sum;
    })->then(function($result) {
        echo "Calculation finished: $result\n";
    })->catch(function($err) {
        echo "Calculation failed!\n";
    })->finally(function() {
        echo "Task cleanup.\n";
    });

    $res->send("Task scheduled in background.");
});

// Example 3: Non-blocking HTTP Request
$app->get('/proxy-status', function ($req, $res) {
    $http = new Kislay\Core\AsyncHttp();
    $http->get("http://127.0.0.1:8081/api/status");
    
    echo "Performing non-blocking HTTP request...\n";
    
    $http->executeAsync()->then(function() use ($http, $res) {
        $code = $http->getResponseCode();
        $body = $http->getResponse();
        echo "Async HTTP Finished with status $code\n";
        // Note: In real production async flows, consider response streaming or shared state
    });

    $res->json(['message' => 'HTTP request is being handled asynchronously']);
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
    echo   "Inspecting request...\n";
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
$app->listen('0.0.0.0', 8081);

// HTTPS example (requires civetweb built with OpenSSL)
// $app->listen('0.0.0.0', 8443, ['cert' => '/path/to/cert.pem', 'key' => '/path/to/key.pem']);

?>