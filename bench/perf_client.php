<?php
/**
 * KislayPHP Core – Performance Benchmark Client
 *
 * Runs a suite of scenarios against a running perf_server.php instance and
 * reports throughput (req/s), average latency, p95 latency, and error rate.
 *
 * Usage:
 *   # Start server first (extension loads from php.ini automatically):
 *   php bench/perf_server.php &
 *
 *   # Run all scenarios (defaults: 127.0.0.1:9191, concurrency 10, 1000 req each):
 *   php bench/perf_client.php
 *
 *   # Custom host/port/concurrency/requests:
 *   php bench/perf_client.php 127.0.0.1 9191 20 5000
 *
 *   # Single scenario by name:
 *   php bench/perf_client.php 127.0.0.1 9191 10 1000 json_small
 *
 * NOTE: Do NOT use -d extension=... — loading the extension twice causes heap corruption.
 */

$host        = $argv[1] ?? '127.0.0.1';
$port        = (int)($argv[2] ?? 9191);
$concurrency = (int)($argv[3] ?? 10);
$requests    = (int)($argv[4] ?? 1000);
$only        = $argv[5] ?? null;       // optional: run single scenario

$baseUrl = "http://{$host}:{$port}";

// ── Helpers ───────────────────────────────────────────────────────────────────

/**
 * Execute $totalRequests HTTP requests with $concurrency parallel curl handles.
 * Returns ['rps' => float, 'avg_ms' => float, 'p95_ms' => float, 'errors' => int, 'total' => int].
 */
function run_scenario(string $url, int $concurrency, int $totalRequests,
                      string $method = 'GET', string $body = '',
                      array $extraHeaders = [], bool $allow5xx = false): array
{
    $latencies = [];
    $errors    = 0;
    $completed = 0;

    $mh      = curl_multi_init();
    $handles = [];
    $startTimes = [];

    $addHandle = function () use ($mh, $url, $method, $body, $extraHeaders, &$handles, &$startTimes) {
        $ch = curl_init($url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 10);
        curl_setopt($ch, CURLOPT_FOLLOWLOCATION, false);

        if ($method === 'POST' || $method === 'PUT') {
            curl_setopt($ch, CURLOPT_CUSTOMREQUEST, $method);
            curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
            $extraHeaders[] = 'Content-Type: application/json';
        }

        if ($extraHeaders) {
            curl_setopt($ch, CURLOPT_HTTPHEADER, $extraHeaders);
        }

        $id = (int)$ch;
        $startTimes[$id] = microtime(true);
        $handles[$id] = $ch;
        curl_multi_add_handle($mh, $ch);
    };

    // Seed initial batch
    $toSend = min($concurrency, $totalRequests);
    for ($i = 0; $i < $toSend; $i++) {
        $addHandle();
    }

    $active = null;
    do {
        while (($s = curl_multi_exec($mh, $active)) === CURLM_CALL_MULTI_PERFORM) {}
        if ($s !== CURLM_OK) {
            break;
        }

        while ($info = curl_multi_info_read($mh)) {
            $ch  = $info['handle'];
            $id  = (int)$ch;
            $lat = (microtime(true) - ($startTimes[$id] ?? microtime(true))) * 1000;

            if ($info['result'] !== CURLE_OK || (!$allow5xx && curl_getinfo($ch, CURLINFO_HTTP_CODE) >= 500)) {
                $errors++;
            } else {
                $latencies[] = $lat;
            }

            $completed++;
            curl_multi_remove_handle($mh, $ch);
            @curl_close($ch);
            unset($handles[$id], $startTimes[$id]);

            if (($completed + count($handles)) < $totalRequests) {
                $addHandle();
            }
        }

        if ($active) {
            curl_multi_select($mh, 0.01);
        }
    } while ($active || $handles);

    curl_multi_close($mh);

    sort($latencies);
    $n   = count($latencies);
    $sum = array_sum($latencies);

    return [
        'rps'    => $n > 0 ? ($n / ($sum / 1000 / $concurrency)) : 0.0,
        'avg_ms' => $n > 0 ? $sum / $n : 0.0,
        'p95_ms' => $n > 0 ? $latencies[(int)($n * 0.95)] : 0.0,
        'errors' => $errors,
        'total'  => $completed,
    ];
}

/** Warm up a URL with a few requests before measuring. */
function warmup(string $url, int $n = 20): void
{
    $mh = curl_multi_init();
    $handles = [];
    for ($i = 0; $i < $n; $i++) {
        $ch = curl_init($url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 5);
        $handles[] = $ch;
        curl_multi_add_handle($mh, $ch);
    }
    $active = null;
    do {
        while (curl_multi_exec($mh, $active) === CURLM_CALL_MULTI_PERFORM) {}
        if ($active) curl_multi_select($mh, 0.01);
    } while ($active);
    foreach ($handles as $ch) {
        curl_multi_remove_handle($mh, $ch);
        @curl_close($ch);
    }
    curl_multi_close($mh);
}

function print_header(): void
{
    printf(
        "\n%-35s | %9s | %9s | %9s | %8s | %6s\n",
        'Scenario', 'req/s', 'avg ms', 'p95 ms', 'errors', 'total'
    );
    echo str_repeat('-', 90) . "\n";
}

function print_result(string $name, array $r): void
{
    $errTag = $r['errors'] > 0 ? sprintf(' (%d ERR)', $r['errors']) : '';
    printf(
        "%-35s | %9.1f | %9.3f | %9.3f | %8s | %6d\n",
        $name,
        $r['rps'],
        $r['avg_ms'],
        $r['p95_ms'],
        $r['errors'] . $errTag,
        $r['total']
    );
}

// ── Verify server is reachable ────────────────────────────────────────────────
$ch = curl_init("$baseUrl/health");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 5);
$body = curl_exec($ch);
$code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
@curl_close($ch);

if ($code !== 200 || trim($body) !== 'ok') {
    fwrite(STDERR, "[perf_client] ERROR: Server not reachable at $baseUrl/health (HTTP $code)\n");
    fwrite(STDERR, "Start it with: php bench/perf_server.php\n");
    exit(1);
}

// ── Scenario registry ─────────────────────────────────────────────────────────
$scenarios = [
    // --- Baseline ---
    'plaintext'               => ['GET',  "$baseUrl/plaintext",          ''],
    'health_check'            => ['GET',  "$baseUrl/health",             ''],

    // --- JSON responses ---
    'json_small'              => ['GET',  "$baseUrl/json/small",         ''],
    'json_1k'                 => ['GET',  "$baseUrl/json/1k",            ''],
    'json_10k'                => ['GET',  "$baseUrl/json/10k",           ''],
    'json_100k'               => ['GET',  "$baseUrl/json/100k",          ''],

    // --- JSON request body ---
    'echo_json_body'          => ['POST', "$baseUrl/echo/json",
                                  json_encode(['user' => 'test', 'value' => str_repeat('x', 200)])],

    // --- Route parameters ---
    'param_1_segment'         => ['GET',  "$baseUrl/users/42",           ''],
    'param_2_segments'        => ['GET',  "$baseUrl/users/42/posts/99",  ''],
    'param_3_segments'        => ['GET',  "$baseUrl/orgs/acme/repos/api/commits/deadbeef", ''],

    // --- Query string ---
    'query_string'            => ['GET',  "$baseUrl/search?q=hello&page=2&limit=50", ''],

    // --- Headers ---
    'header_read'             => ['GET',  "$baseUrl/headers/read",       '',
                                  ['Authorization: Bearer eyJhbGciOiJIUzI1NiJ9',
                                   'Accept: application/json',
                                   'User-Agent: KislayBench/1.0']],
    'header_write_4x'         => ['GET',  "$baseUrl/headers/write",      ''],

    // --- Attributes ---
    'request_attributes'      => ['GET',  "$baseUrl/attrs",              ''],

    // --- Middleware chains ---
    'middleware_with_use'     => ['GET',  "$baseUrl/mw/1",               ''],
    'middleware_plain'        => ['GET',  "$baseUrl/mw/plain",           ''],

    // --- Groups ---
    'group_plain'             => ['GET',  "$baseUrl/api/ping",           ''],
    'group_param'             => ['GET',  "$baseUrl/api/users/77/profile", ''],

    // --- Async ---
    'async_fire_and_forget'   => ['GET',  "$baseUrl/async/fire",         ''],
    'async_promise_chain'     => ['GET',  "$baseUrl/async/chain",        ''],

    // --- File serving ---
    'file_1k'                 => ['GET',  "$baseUrl/file/1k",            ''],
    'file_10k'                => ['GET',  "$baseUrl/file/10k",           ''],
    'file_100k'               => ['GET',  "$baseUrl/file/100k",          ''],

    // --- Status helpers ---
    'status_200_ok'           => ['GET',  "$baseUrl/status/200",         ''],
    'status_400_bad_request'  => ['GET',  "$baseUrl/status/400",         ''],
    'status_404_not_found'    => ['GET',  "$baseUrl/status/404",         ''],
    // NOTE: status_500 intentionally returns HTTP 500; allow5xx=true counts them as valid samples
    'status_500_error'        => ['GET',  "$baseUrl/status/500",         '', [], true],
];

// ── Run ───────────────────────────────────────────────────────────────────────
echo "\n══════════════════════════════════════════════════════════════════════════\n";
echo " KislayPHP Core – Performance Benchmark\n";
echo "══════════════════════════════════════════════════════════════════════════\n";
printf(" Server      : %s\n", $baseUrl);
printf(" Concurrency : %d\n", $concurrency);
printf(" Requests    : %d per scenario\n", $requests);
if ($only) {
    printf(" Filter      : %s\n", $only);
}
echo "══════════════════════════════════════════════════════════════════════════\n";

// Groups for section headers
$sections = [
    'Baseline'           => ['plaintext', 'health_check'],
    'JSON Responses'     => ['json_small', 'json_1k', 'json_10k', 'json_100k'],
    'JSON Request Body'  => ['echo_json_body'],
    'Route Parameters'   => ['param_1_segment', 'param_2_segments', 'param_3_segments'],
    'Query Strings'      => ['query_string'],
    'Headers'            => ['header_read', 'header_write_4x'],
    'Request Attributes' => ['request_attributes'],
    'Middleware Chains'  => ['middleware_with_use', 'middleware_plain'],
    'Route Groups'       => ['group_plain', 'group_param'],
    'Async / Promises'   => ['async_fire_and_forget', 'async_promise_chain'],
    'File Serving'       => ['file_1k', 'file_10k', 'file_100k'],
    'Status Helpers'     => ['status_200_ok', 'status_400_bad_request', 'status_404_not_found', 'status_500_error'],
];

$summary = [];

foreach ($sections as $sectionName => $keys) {
    $printed = false;
    foreach ($keys as $key) {
        if ($only && $key !== $only) {
            continue;
        }

        if (!isset($scenarios[$key])) {
            continue;
        }

        if (!$printed) {
            print_header();
            echo "  ▶ $sectionName\n";
            $printed = true;
        }

        [$method, $url, $body] = $scenarios[$key];
        $extraHeaders = $scenarios[$key][3] ?? [];
        $allow5xx     = (bool)($scenarios[$key][4] ?? false);

        // Warm up
        warmup($url, 20);

        // Measure
        $result = run_scenario($url, $concurrency, $requests, $method, $body, $extraHeaders, $allow5xx);
        print_result("  $key", $result);
        $summary[$key] = $result;
    }
}

// ── Summary table ─────────────────────────────────────────────────────────────
if (count($summary) > 1) {
    echo "\n══════════════════════════════════════════════════════════════════════════\n";
    echo " Summary – sorted by throughput (req/s)\n";
    echo "══════════════════════════════════════════════════════════════════════════\n";
    print_header();
    uasort($summary, fn($a, $b) => $b['rps'] <=> $a['rps']);
    $rank = 1;
    foreach ($summary as $k => $r) {
        print_result(sprintf("  #%-2d %s", $rank++, $k), $r);
    }

    // Fastest vs slowest
    reset($summary);
    $fastest = key($summary);
    end($summary);
    $slowest = key($summary);
    echo "\n";
    printf("  Fastest: %-30s  %.1f req/s\n", $fastest, $summary[$fastest]['rps']);
    printf("  Slowest: %-30s  %.1f req/s\n", $slowest, $summary[$slowest]['rps']);
}

echo "\n[perf_client] Done.\n";
