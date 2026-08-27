<?php
/**
 * KislayPHP Core – Performance Benchmark Server
 *
 * Covers:
 *   - Plaintext / JSON / XML response throughput
 *   - Route parameter extraction
 *   - Query-string parsing
 *   - Header reading & writing
 *   - JSON request-body parsing
 *   - Middleware chain overhead (1 / 5 / 10 layers)
 *   - Nested route groups
 *   - Attribute storage round-trip
 *   - File serving
 *   - Async task dispatch
 *   - Response status helpers (4xx / 5xx)
 *   - Large JSON payload (1 KB / 10 KB / 100 KB)
 *
 * Usage:
 *   php bench/perf_server.php
 *
 * Then run the companion client:
 *   php bench/perf_client.php [host] [port]
 *
 * The kislayphp_extension is installed globally via php.ini.
 * Do NOT pass -d extension=... — loading it twice causes heap corruption.
 */

if (!extension_loaded('kislayphp_extension')) {
    fwrite(STDERR, "[perf_server] ERROR: kislayphp_extension not loaded.\n");
    fwrite(STDERR, "  The extension should be installed in: " . ini_get('extension_dir') . "\n");
    fwrite(STDERR, "  Check: php -m | grep kislayphp_extension\n");
    exit(1);
}

$host = getenv('BENCH_HOST') ?: '127.0.0.1';
$port = (int)(getenv('BENCH_PORT') ?: 9191);

$app = new Kislay\Core\App();
$app->setOption('log', false);
$serverType = getenv('BENCH_SERVER_TYPE');
if ($serverType) {
    $app->setOption('server_type', $serverType);
}

// ── Static files ──────────────────────────────────────────────────────────────
$staticDir = __DIR__ . '/static';
if (!is_dir($staticDir)) {
    mkdir($staticDir, 0755, true);
}

foreach ([1, 10, 100] as $kb) {
    $f = "$staticDir/payload_{$kb}kb.bin";
    if (!file_exists($f)) {
        file_put_contents($f, str_repeat('A', $kb * 1024));
    }
}

// ── Prebuilt payloads (avoid per-request allocations in PHP land) ─────────────
$json1k   = json_encode(['status' => 'ok', 'data' => str_repeat('x', 950)]);
$json10k  = json_encode(['status' => 'ok', 'data' => str_repeat('x', 9950)]);
$json100k = json_encode(['status' => 'ok', 'data' => str_repeat('x', 99950)]);

// ── Helpers ───────────────────────────────────────────────────────────────────
$noop = function ($req, $res) { return true; };

// ── Routes: baseline ──────────────────────────────────────────────────────────
$app->get('/health', function ($req, $res) {
    $res->send('ok');
});

$app->get('/plaintext', function ($req, $res) {
    $res->send('Hello from KislayPHP');
});

// ── Routes: JSON ──────────────────────────────────────────────────────────────
$app->get('/json/small', function ($req, $res) {
    $res->json(['status' => 'ok', 'ts' => microtime(true)]);
});

$app->get('/json/1k', function ($req, $res) use ($json1k) {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json1k);
});

$app->get('/json/10k', function ($req, $res) use ($json10k) {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json10k);
});

$app->get('/json/100k', function ($req, $res) use ($json100k) {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json100k);
});

// ── Routes: JSON request body parse ───────────────────────────────────────────
$app->post('/echo/json', function ($req, $res) {
    $data = $req->json();
    $res->json(['received' => count((array)$data)]);
});

// ── Routes: route parameters ──────────────────────────────────────────────────
$app->get('/users/:id', function ($req, $res) {
    $res->json(['id' => $req->param('id')]);
});

$app->get('/users/:id/posts/:postId', function ($req, $res) {
    $res->json([
        'userId' => $req->param('id'),
        'postId' => $req->param('postId'),
    ]);
});

$app->get('/orgs/:org/repos/:repo/commits/:sha', function ($req, $res) {
    $res->json([
        'org'  => $req->param('org'),
        'repo' => $req->param('repo'),
        'sha'  => $req->param('sha'),
    ]);
});

// ── Routes: query-string ──────────────────────────────────────────────────────
$app->get('/search', function ($req, $res) {
    $q     = $req->query('q', '');
    $page  = (int) $req->query('page', 1);
    $limit = (int) $req->query('limit', 20);
    $res->json(['q' => $q, 'page' => $page, 'limit' => $limit, 'results' => []]);
});

// ── Routes: header reading ────────────────────────────────────────────────────
$app->get('/headers/read', function ($req, $res) {
    $auth    = $req->header('Authorization', '');
    $accept  = $req->header('Accept', '*/*');
    $ua      = $req->header('User-Agent', '');
    $res->json(['auth_len' => strlen($auth), 'accept' => $accept, 'ua_len' => strlen($ua)]);
});

// ── Routes: response headers ──────────────────────────────────────────────────
$app->get('/headers/write', function ($req, $res) {
    $res->setHeader('X-Request-Id', uniqid('req-', true));
    $res->setHeader('X-Custom-One', 'alpha');
    $res->setHeader('X-Custom-Two', 'beta');
    $res->setHeader('Cache-Control', 'no-store');
    $res->send('headers-written');
});

// ── Routes: request attributes ────────────────────────────────────────────────
$app->get('/attrs', function ($req, $res) {
    $req->setAttribute('user_id', 42);
    $req->setAttribute('role', 'admin');
    $req->setAttribute('meta', ['a' => 1, 'b' => 2]);
    $res->json([
        'uid'  => $req->getAttribute('user_id'),
        'role' => $req->getAttribute('role'),
    ]);
});

// ── Routes: middleware chain ──────────────────────────────────────────────────
// App::get() accepts exactly 2 args (path, handler). Middleware is registered
// with $app->use($prefix, $fn) and runs before all routes under that prefix.

// Measure overhead of 1 scoped middleware:
$app->use('/mw', function ($req, $res) {
    // lightweight auth-stub: read one header, set one attr
    $req->setAttribute('mw1', true);
    return true;
});
$app->get('/mw/1', function ($req, $res) { $res->send('mw1'); });

// Measure overhead of middleware + group nesting:
$app->get('/mw/plain', function ($req, $res) { $res->send('plain'); });

// ── Routes: scoped middleware via group ───────────────────────────────────────
$apiMiddleware = [function ($req, $res) {
    $res->setHeader('X-Api-Version', '1');
    return true;
}];
$app->group('/api', function ($router) {
    $router->get('/ping', function ($req, $res) {
        $res->send('pong');
    });
    $router->get('/users/:id/profile', function ($req, $res) {
        $res->json(['id' => $req->param('id'), 'scope' => 'api']);
    });
}, $apiMiddleware);

// ── Routes: async task dispatch ───────────────────────────────────────────────
$app->get('/async/fire', function ($req, $res) {
    async(function () {
        return 42;
    });
    $res->send('fired');
});

$app->get('/async/chain', function ($req, $res) {
    $p = async(function () { return 1; });
    $p->then(function ($v) { return $v + 1; })
      ->then(function ($v) { return $v + 1; });
    $res->json(['dispatched' => true]);
});

// ── Routes: file serving ─────────────────────────────────────────────────────
$app->get('/file/1k', function ($req, $res) use ($staticDir) {
    $res->sendFile("$staticDir/payload_1kb.bin");
});

$app->get('/file/10k', function ($req, $res) use ($staticDir) {
    $res->sendFile("$staticDir/payload_10kb.bin");
});

$app->get('/file/100k', function ($req, $res) use ($staticDir) {
    $res->sendFile("$staticDir/payload_100kb.bin");
});

// ── Routes: status helpers ────────────────────────────────────────────────────
$app->get('/status/200', function ($req, $res) { $res->ok('OK'); });
$app->get('/status/201', function ($req, $res) { $res->created(['id' => 1]); });
$app->get('/status/400', function ($req, $res) { $res->badRequest('Bad input'); });
$app->get('/status/401', function ($req, $res) { $res->unauthorized('Unauthorized'); });
$app->get('/status/403', function ($req, $res) { $res->forbidden('Forbidden'); });
$app->get('/status/404', function ($req, $res) { $res->notFound('Not found'); });
$app->get('/status/500', function ($req, $res) { $res->internalServerError('Error'); });

// ── Start ─────────────────────────────────────────────────────────────────────
fwrite(STDOUT, "[perf_server] Listening on http://{$host}:{$port}\n");
fwrite(STDOUT, "[perf_server] Routes registered – run perf_client.php to benchmark\n");

$app->listen($host, $port);
