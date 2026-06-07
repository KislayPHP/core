<?php
/**
 * ╔══════════════════════════════════════════════════════════════════════════════╗
 * ║         KislayPHP — Performance Benchmark: Annotated Code Samples           ║
 * ║                                                                              ║
 * ║  Every scenario measured by perf_client.php is shown here with the exact    ║
 * ║  kislayphp API call that produces it.  The number next to each section      ║
 * ║  is the scenario key from perf_client.php.                                  ║
 * ║                                                                              ║
 * ║  This file is NOT meant to be run directly — it is a reference.             ║
 * ║  Run the actual benchmark with:                                              ║
 * ║      ./run_perf.sh quick      ← fast (10 concurrent, 500 req/scenario)     ║
 * ║      ./run_perf.sh full       ← full (20 concurrent, 2000 req/scenario)    ║
 * ╚══════════════════════════════════════════════════════════════════════════════╝
 */

// ── Bootstrap (shared by all samples) ────────────────────────────────────────
// The extension is loaded via:  php -n -d extension=/path/to/kislayphp_extension.so

$app = new Kislay\Core\App();
$app->setOption('log',         false);   // disable access log for bench
$app->setOption('num_threads', 8);       // CivetWeb HTTP worker threads
// $app->setOption('workers', 10);       // optional: PHP-side worker pool


// ════════════════════════════════════════════════════════════════════════════
// § 1  BASELINE
// ════════════════════════════════════════════════════════════════════════════

// [plaintext] — raw throughput floor; no allocation, no encoding
// Measures: routing + response write path cost
$app->get('/plaintext', function ($req, $res): void {
    $res->send('Hello from KislayPHP');       // sets text/html by default
});

// [health_check] — 2-byte body; used as the "is server alive?" probe
$app->get('/health', function ($req, $res): void {
    $res->send('ok');
});


// ════════════════════════════════════════════════════════════════════════════
// § 2  JSON RESPONSES — measures json_encode + header write
// ════════════════════════════════════════════════════════════════════════════

// [json_small] — ~60 B payload; typical REST acknowledgement
$app->get('/json/small', function ($req, $res): void {
    $res->json(['status' => 'ok', 'ts' => microtime(true)]);
    //  ↑ automatically sets Content-Type: application/json
});

// [json_1k / json_10k / json_100k] — pre-built strings, zero per-request cost
$json1k   = json_encode(['status' => 'ok', 'data' => str_repeat('x', 950)]);
$json10k  = json_encode(['status' => 'ok', 'data' => str_repeat('x', 9950)]);
$json100k = json_encode(['status' => 'ok', 'data' => str_repeat('x', 99950)]);

$app->get('/json/1k',   function ($req, $res) use ($json1k):   void {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json1k);
});
$app->get('/json/10k',  function ($req, $res) use ($json10k):  void {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json10k);
});
$app->get('/json/100k', function ($req, $res) use ($json100k): void {
    $res->setHeader('Content-Type', 'application/json');
    $res->send($json100k);
});


// ════════════════════════════════════════════════════════════════════════════
// § 3  JSON REQUEST BODY — measures body read + json_decode
// ════════════════════════════════════════════════════════════════════════════

// [echo_json_body] — 200-byte POST body round-trip
// Client sends: {"user":"test","value":"xxxx..."}
$app->post('/echo/json', function ($req, $res): void {
    $data = $req->json();                        // decode application/json body
    $res->json(['received' => count((array) $data)]);
});


// ════════════════════════════════════════════════════════════════════════════
// § 4  ROUTE PARAMETERS — measures trie traversal + param extraction
// ════════════════════════════════════════════════════════════════════════════

// [param_1_segment]   GET /users/42
$app->get('/users/:id', function ($req, $res): void {
    $res->json(['id' => $req->param('id')]);
});

// [param_2_segments]  GET /users/42/posts/99
$app->get('/users/:id/posts/:postId', function ($req, $res): void {
    $res->json([
        'userId' => $req->param('id'),
        'postId' => $req->param('postId'),
    ]);
});

// [param_3_segments]  GET /orgs/acme/repos/api/commits/deadbeef
$app->get('/orgs/:org/repos/:repo/commits/:sha', function ($req, $res): void {
    $res->json([
        'org'  => $req->param('org'),
        'repo' => $req->param('repo'),
        'sha'  => $req->param('sha'),
    ]);
});


// ════════════════════════════════════════════════════════════════════════════
// § 5  QUERY STRINGS — measures URL-decode + key lookup
// ════════════════════════════════════════════════════════════════════════════

// [query_string]  GET /search?q=hello&page=2&limit=50
$app->get('/search', function ($req, $res): void {
    $q     = $req->query('q',     '');
    $page  = (int) $req->query('page',  1);
    $limit = (int) $req->query('limit', 20);
    $res->json(['q' => $q, 'page' => $page, 'limit' => $limit, 'results' => []]);
});


// ════════════════════════════════════════════════════════════════════════════
// § 6  HEADERS — measures header read/write overhead
// ════════════════════════════════════════════════════════════════════════════

// [header_read]  client sends Authorization + Accept + User-Agent
$app->get('/headers/read', function ($req, $res): void {
    $auth   = $req->header('Authorization', '');
    $accept = $req->header('Accept',        '*/*');
    $ua     = $req->header('User-Agent',    '');
    $res->json(['auth_len' => strlen($auth), 'accept' => $accept, 'ua_len' => strlen($ua)]);
});

// [header_write_4x]  adds 4 response headers per request
$app->get('/headers/write', function ($req, $res): void {
    $res->setHeader('X-Request-Id',  uniqid('req-', true));
    $res->setHeader('X-Custom-One',  'alpha');
    $res->setHeader('X-Custom-Two',  'beta');
    $res->setHeader('Cache-Control', 'no-store');
    $res->send('headers-written');
});


// ════════════════════════════════════════════════════════════════════════════
// § 7  REQUEST ATTRIBUTES — measures per-request hashmap (set + get)
// ════════════════════════════════════════════════════════════════════════════

// [request_attributes]  write 3 attrs, read 2 back
$app->get('/attrs', function ($req, $res): void {
    $req->setAttribute('user_id', 42);
    $req->setAttribute('role',    'admin');
    $req->setAttribute('meta',    ['a' => 1, 'b' => 2]);

    $res->json([
        'uid'  => $req->getAttribute('user_id'),
        'role' => $req->getAttribute('role'),
    ]);
});


// ════════════════════════════════════════════════════════════════════════════
// § 8  MIDDLEWARE CHAINS — measures per-request overhead of use() callbacks
// ════════════════════════════════════════════════════════════════════════════

// [middleware_with_use]  1 scoped middleware via $app->use()
// use() registers a middleware that runs before every route under the prefix.
$app->use('/mw', function ($req, $res): bool {
    $req->setAttribute('mw1', true);   // lightweight auth-stub
    return true;                        // true = continue to next handler
});
$app->get('/mw/1',     function ($req, $res): void { $res->send('mw1'); });

// [middleware_plain]  same prefix, no middleware cost (measures routing only)
$app->get('/mw/plain', function ($req, $res): void { $res->send('plain'); });


// ════════════════════════════════════════════════════════════════════════════
// § 9  ROUTE GROUPS — measures group prefix matching + scoped middleware
// ════════════════════════════════════════════════════════════════════════════

// [group_plain / group_param]
// group() registers all inner routes under /api and applies $apiMiddleware.
$apiMiddleware = [function ($req, $res): bool {
    $res->setHeader('X-Api-Version', '1');
    return true;
}];

$app->group('/api', function ($router): void {
    // [group_plain]  GET /api/ping
    $router->get('/ping', function ($req, $res): void {
        $res->send('pong');
    });

    // [group_param]  GET /api/users/77/profile
    $router->get('/users/:id/profile', function ($req, $res): void {
        $res->json(['id' => $req->param('id'), 'scope' => 'api']);
    });
}, $apiMiddleware);


// ════════════════════════════════════════════════════════════════════════════
// § 10  ASYNC / PROMISES — measures task dispatch overhead
// ════════════════════════════════════════════════════════════════════════════

// [async_fire_and_forget]  dispatch a background task, respond immediately
$app->get('/async/fire', function ($req, $res): void {
    async(function (): int {     // runs asynchronously; return value ignored
        return 42;
    });
    $res->send('fired');         // response sent before task completes
});

// [async_promise_chain]  build a 3-step promise chain, then respond
$app->get('/async/chain', function ($req, $res): void {
    $p = async(function (): int { return 1; });
    $p->then(function (int $v): int { return $v + 1; })
      ->then(function (int $v): int { return $v + 1; });   // resolves to 3
    $res->json(['dispatched' => true]);
});


// ════════════════════════════════════════════════════════════════════════════
// § 11  FILE SERVING — measures sendFile() + OS read + Content-Length header
// ════════════════════════════════════════════════════════════════════════════

$staticDir = __DIR__ . '/static';
is_dir($staticDir) || mkdir($staticDir, 0755, true);

// Pre-create static files (1 KB / 10 KB / 100 KB)
foreach ([1, 10, 100] as $kb) {
    $f = "$staticDir/payload_{$kb}kb.bin";
    is_file($f) || file_put_contents($f, str_repeat('A', $kb * 1024));
}

// [file_1k / file_10k / file_100k]
$app->get('/file/1k',   function ($req, $res) use ($staticDir): void {
    $res->sendFile("$staticDir/payload_1kb.bin");   // sets Content-Length automatically
});
$app->get('/file/10k',  function ($req, $res) use ($staticDir): void {
    $res->sendFile("$staticDir/payload_10kb.bin");
});
$app->get('/file/100k', function ($req, $res) use ($staticDir): void {
    $res->sendFile("$staticDir/payload_100kb.bin");
});


// ════════════════════════════════════════════════════════════════════════════
// § 12  STATUS HELPERS — measures named response shortcuts
// ════════════════════════════════════════════════════════════════════════════

// [status_200_ok / status_400_bad_request / status_404_not_found / status_500_error]
// These measure the cost of the helper method vs. manual status() + send().
$app->get('/status/200', function ($req, $res): void { $res->ok('OK'); });
$app->get('/status/201', function ($req, $res): void { $res->created(['id' => 1]); });
$app->get('/status/400', function ($req, $res): void { $res->badRequest('Bad input'); });
$app->get('/status/401', function ($req, $res): void { $res->unauthorized('Unauthorized'); });
$app->get('/status/403', function ($req, $res): void { $res->forbidden('Forbidden'); });
$app->get('/status/404', function ($req, $res): void { $res->notFound('Not found'); });
$app->get('/status/500', function ($req, $res): void { $res->internalServerError('Error'); });


// ════════════════════════════════════════════════════════════════════════════
// § 13  SPRING-BOOT-STYLE DI + CONFIG PATTERN (non-HTTP overhead reference)
// ════════════════════════════════════════════════════════════════════════════
// The following is NOT benchmarked in the HTTP suite (it's an app-startup
// pattern), but is shown here to demonstrate the full kislayphp API surface.

/*
// ConfigurationProperties (populated from env / config file at startup):
class AppConfig
{
    public string $dbHost   = 'localhost';
    public int    $dbPort   = 5432;
    public string $jwtSecret = '';
    public int    $maxPool  = 10;
}

$config = new AppConfig();
// … load from env / YAML / kislayphp config module …

// DI container wiring (done once at startup, zero overhead per-request):
$container = new Kislay\DI\Container();
$container->bind('db',      fn() => new PDO("pgsql:host={$config->dbHost}", 'u', 'p'));
$container->bind('mailer',  fn() => new Mailer($config));
$container->singleton('auth', fn() => new AuthService($container->get('db'), $config->jwtSecret));

// Route using resolved service (container resolves singletons once):
$app->get('/me', function ($req, $res) use ($container): void {
    $auth = $container->get('auth');
    $user = $auth->fromToken($req->header('Authorization', ''));
    $res->json($user ?? []);
});

// @Scheduled equivalent — cron expression, runs in background:
$app->schedule('0 * * * *', function (): void {
    // runs every hour; zero HTTP overhead
    echo "Hourly job\n";
});
*/


// ════════════════════════════════════════════════════════════════════════════
// § 14  LISTEN (blocking — starts CivetWeb + PHP worker threads)
// ════════════════════════════════════════════════════════════════════════════

// This is the last call — it blocks until SIGTERM / SIGINT.
// For the benchmark, run_perf.sh starts this as a background process.
$host = getenv('BENCH_HOST') ?: '127.0.0.1';
$port = (int)(getenv('BENCH_PORT') ?: 9191);

fwrite(STDOUT, "[perf_code_samples] Listening on http://{$host}:{$port}\n");
$app->listen($host, $port);
