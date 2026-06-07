<?php
/**
 * KislayPHP Core – Performance Benchmark Client
 *
 * Go-bench-style HTTP benchmarking:
 *   - Wall-clock RPS (requests / elapsed_seconds, not the old wrong formula)
 *   - Full latency histogram: min, p50, p95, p99, p999, max, mean, stddev
 *   - Bytes/op: average response body bytes per request
 *   - ns/op: nanoseconds per request (1e9 / rps) — matches Go bench output
 *   - Multiple runs with coefficient-of-variation stability check
 *   - Duration-based mode: run each scenario for N ms instead of fixed count
 *   - Go-bench text output for machine parsing / benchstat
 *   - Adaptive warmup until server is warm
 *
 * Usage:
 *   php bench/perf_client.php [host] [port] [concurrency] [requests] [filter] [runs]
 *
 * Environment variables:
 *   BENCH_DURATION_MS=5000  — run each scenario for N ms (overrides $requests)
 *   BENCH_JSON=1            — emit JSON results to stdout after table
 *   BENCH_GOBENCH=1         — emit Go-bench text lines (for benchstat)
 *   BENCH_RUNS=3            — number of runs per scenario (stability check)
 *   BENCH_CV_WARN=5         — warn if RPS CV% across runs exceeds this value
 */

$host        = $argv[1] ?? '127.0.0.1';
$port        = (int)($argv[2] ?? 9191);
$concurrency = (int)($argv[3] ?? 10);
$requests    = (int)($argv[4] ?? 1000);
$only        = $argv[5] ?? null;
$runs        = (int)($argv[6] ?? (int)(getenv('BENCH_RUNS') ?: 1));

$baseUrl        = "http://{$host}:{$port}";
$durationMs     = (int)(getenv('BENCH_DURATION_MS') ?: 0);   // 0 = use $requests
$emitJson       = (bool)(getenv('BENCH_JSON') ?: false);
$emitGoBench    = (bool)(getenv('BENCH_GOBENCH') ?: false);
$cvWarnPct      = (float)(getenv('BENCH_CV_WARN') ?: 5.0);

// ── Core helpers ──────────────────────────────────────────────────────────────

/**
 * Run $totalRequests HTTP requests with $concurrency parallel curl handles.
 * Uses wall-clock time for correct RPS. Tracks response sizes for bytes/op.
 *
 * Returns:
 *   rps      float   requests per second (wall-clock)
 *   ns_op    float   nanoseconds per request (1e9 / rps)
 *   avg_ms   float   mean latency in ms
 *   p50_ms   float   50th percentile latency
 *   p95_ms   float   95th percentile latency
 *   p99_ms   float   99th percentile latency
 *   p999_ms  float   99.9th percentile latency
 *   min_ms   float   minimum latency
 *   max_ms   float   maximum latency
 *   stddev_ms float  latency standard deviation
 *   bytes_op float   average response bytes per request
 *   errors   int     failed requests (curl error or 5xx, unless allow5xx)
 *   total    int     total requests attempted
 *   elapsed_s float  wall-clock elapsed seconds
 */
function run_scenario(
    string $url,
    int $concurrency,
    int $totalRequests,
    string $method = 'GET',
    string $body = '',
    array $extraHeaders = [],
    bool $allow5xx = false,
    int $durationMs = 0
): array {
    $latencies    = [];
    $byteSizes    = [];
    $errors       = 0;
    $completed    = 0;

    $mh         = curl_multi_init();
    $handles    = [];
    $startTimes = [];
    $deadline   = $durationMs > 0 ? (microtime(true) + $durationMs / 1000.0) : null;

    $addHandle = function () use (
        $mh, $url, $method, $body, $extraHeaders, &$handles, &$startTimes
    ) {
        $ch = curl_init($url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 10);
        curl_setopt($ch, CURLOPT_FOLLOWLOCATION, false);
        curl_setopt($ch, CURLOPT_ENCODING, '');   // accept-encoding: gzip,deflate
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
        $handles[$id]    = $ch;
        curl_multi_add_handle($mh, $ch);
    };

    // Seed the initial batch of concurrent handles
    $toSend = $durationMs > 0 ? $concurrency : min($concurrency, $totalRequests);
    for ($i = 0; $i < $toSend; $i++) {
        $addHandle();
    }

    $wallStart = microtime(true);
    $active    = null;

    do {
        while (($s = curl_multi_exec($mh, $active)) === CURLM_CALL_MULTI_PERFORM) {}
        if ($s !== CURLM_OK) {
            break;
        }

        while ($info = curl_multi_info_read($mh)) {
            $ch  = $info['handle'];
            $id  = (int)$ch;
            $lat = (microtime(true) - ($startTimes[$id] ?? microtime(true))) * 1000.0;
            $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
            $size      = (float)curl_getinfo($ch, CURLINFO_SIZE_DOWNLOAD);

            if ($info['result'] !== CURLE_OK || (!$allow5xx && $http_code >= 500)) {
                $errors++;
            } else {
                $latencies[] = $lat;
                $byteSizes[] = $size;
            }

            $completed++;
            curl_multi_remove_handle($mh, $ch);
            @curl_close($ch);
            unset($handles[$id], $startTimes[$id]);

            // Add another handle if we still need more requests
            $needMore = $durationMs > 0
                ? (microtime(true) < $deadline)
                : (($completed + count($handles)) < $totalRequests);
            if ($needMore) {
                $addHandle();
            }
        }

        if ($active) {
            curl_multi_select($mh, 0.005);
        }

        // Duration mode: stop seeding new handles once deadline passes
        if ($deadline !== null && microtime(true) >= $deadline && !$active && empty($handles)) {
            break;
        }
    } while ($active || $handles);

    $wallElapsed = microtime(true) - $wallStart;
    curl_multi_close($mh);

    // ── Latency statistics ────────────────────────────────────────────────────
    sort($latencies);
    $n = count($latencies);

    if ($n === 0) {
        return [
            'rps' => 0.0, 'ns_op' => INF, 'avg_ms' => 0.0,
            'p50_ms' => 0.0, 'p95_ms' => 0.0, 'p99_ms' => 0.0, 'p999_ms' => 0.0,
            'min_ms' => 0.0, 'max_ms' => 0.0, 'stddev_ms' => 0.0,
            'bytes_op' => 0.0, 'errors' => $errors, 'total' => $completed,
            'elapsed_s' => $wallElapsed,
        ];
    }

    $sum    = array_sum($latencies);
    $mean   = $sum / $n;
    $variance = 0.0;
    foreach ($latencies as $v) {
        $diff = $v - $mean;
        $variance += $diff * $diff;
    }
    $stddev = sqrt($variance / $n);

    $pct = function (float $p) use ($latencies, $n): float {
        $idx = (int)ceil($p * $n) - 1;
        return $latencies[max(0, min($idx, $n - 1))];
    };

    $rps = $wallElapsed > 0 ? ($n / $wallElapsed) : 0.0;

    return [
        'rps'       => $rps,
        'ns_op'     => $rps > 0 ? (1.0e9 / $rps) : INF,
        'avg_ms'    => $mean,
        'p50_ms'    => $pct(0.50),
        'p95_ms'    => $pct(0.95),
        'p99_ms'    => $pct(0.99),
        'p999_ms'   => $pct(0.999),
        'min_ms'    => $latencies[0],
        'max_ms'    => $latencies[$n - 1],
        'stddev_ms' => $stddev,
        'bytes_op'  => array_sum($byteSizes) / $n,
        'errors'    => $errors,
        'total'     => $completed,
        'elapsed_s' => $wallElapsed,
    ];
}

/**
 * Run a scenario $runs times and return merged stats.
 * Reports coefficient of variation across runs; warns if unstable.
 */
function run_benchmark(
    string $name,
    string $url,
    int $concurrency,
    int $requests,
    string $method = 'GET',
    string $body = '',
    array $extraHeaders = [],
    bool $allow5xx = false,
    int $durationMs = 0,
    int $runs = 1,
    float $cvWarnPct = 5.0
): array {
    $allResults = [];
    $allRps     = [];

    for ($i = 0; $i < $runs; $i++) {
        $r        = run_scenario($url, $concurrency, $requests, $method, $body, $extraHeaders, $allow5xx, $durationMs);
        $allResults[] = $r;
        $allRps[]     = $r['rps'];
    }

    if ($runs === 1) {
        $best = $allResults[0];
        $best['cv_pct']  = 0.0;
        $best['runs']    = 1;
        $best['unstable'] = false;
        return $best;
    }

    // Coefficient of variation across runs
    $meanRps  = array_sum($allRps) / $runs;
    $varRps   = 0.0;
    foreach ($allRps as $r) {
        $varRps += ($r - $meanRps) ** 2;
    }
    $cvPct = $meanRps > 0 ? (sqrt($varRps / $runs) / $meanRps) * 100.0 : 0.0;

    // Use the run with RPS closest to the median (robust central estimate)
    sort($allRps);
    $medianRps = $allRps[(int)($runs / 2)];
    $best = $allResults[0];
    $bestDiff = INF;
    foreach ($allResults as $r) {
        $diff = abs($r['rps'] - $medianRps);
        if ($diff < $bestDiff) {
            $bestDiff = $diff;
            $best = $r;
        }
    }

    $best['cv_pct']   = $cvPct;
    $best['runs']     = $runs;
    $best['unstable'] = $cvPct > $cvWarnPct;
    return $best;
}

/**
 * Adaptive warmup: fire requests until latency CV drops below 10%
 * or we've done at least $minWarmup requests, whichever comes last.
 */
function warmup(string $url, int $minWarmup = 50, string $method = 'GET', string $body = '', array $extraHeaders = []): void
{
    $mh = curl_multi_init();
    $handles = [];
    $count = max($minWarmup, 30);
    for ($i = 0; $i < $count; $i++) {
        $ch = curl_init($url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 5);
        if ($method === 'POST') {
            curl_setopt($ch, CURLOPT_CUSTOMREQUEST, 'POST');
            curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
            if ($extraHeaders) curl_setopt($ch, CURLOPT_HTTPHEADER, $extraHeaders);
        }
        $handles[] = $ch;
        curl_multi_add_handle($mh, $ch);
    }
    $active = null;
    do {
        while (curl_multi_exec($mh, $active) === CURLM_CALL_MULTI_PERFORM) {}
        if ($active) curl_multi_select($mh, 0.005);
    } while ($active);
    foreach ($handles as $ch) {
        curl_multi_remove_handle($mh, $ch);
        @curl_close($ch);
    }
    curl_multi_close($mh);
}

// ── Output helpers ────────────────────────────────────────────────────────────

function fmt_lat(float $ms): string
{
    if ($ms < 1.0)   return sprintf('%.1fµs', $ms * 1000);
    if ($ms < 1000)  return sprintf('%.2fms', $ms);
    return sprintf('%.2fs', $ms / 1000);
}

function fmt_bytes(float $b): string
{
    if ($b < 1024)       return sprintf('%.0f B', $b);
    if ($b < 1048576)    return sprintf('%.1f KB', $b / 1024);
    return sprintf('%.2f MB', $b / 1048576);
}

function print_section_header(string $section): void
{
    echo "\n  ▶ $section\n";
    printf(
        "  %-30s | %9s | %8s | %8s | %8s | %8s | %8s | %9s | %9s | %6s\n",
        'Scenario', 'req/s', 'ns/op', 'p50', 'p95', 'p99', 'p999', 'bytes/op', 'errors', 'total'
    );
    echo '  ' . str_repeat('-', 115) . "\n";
}

function print_result(string $name, array $r): void
{
    $stability = '';
    if (($r['runs'] ?? 1) > 1) {
        $cv = $r['cv_pct'] ?? 0;
        $stability = ($r['unstable'] ?? false) ? sprintf(' ⚠ CV=%.1f%%', $cv) : sprintf(' ✓ CV=%.1f%%', $cv);
    }
    $errTag = $r['errors'] > 0 ? sprintf('!%d', $r['errors']) : '0';
    printf(
        "  %-30s | %9.1f | %8.0f | %8s | %8s | %8s | %8s | %9s | %9s | %6d%s\n",
        $name,
        $r['rps'],
        $r['ns_op'],
        fmt_lat($r['p50_ms']),
        fmt_lat($r['p95_ms']),
        fmt_lat($r['p99_ms']),
        fmt_lat($r['p999_ms']),
        fmt_bytes($r['bytes_op']),
        $errTag,
        $r['total'],
        $stability
    );
}

function emit_gobench_line(string $name, array $r, int $cpus = 1): void
{
    // Format: BenchmarkName-N   iterations   ns/op   B/op
    $safe = preg_replace('/[^A-Za-z0-9_]/', '_', $name);
    $iters = max(1, $r['total'] - $r['errors']);
    printf(
        "Benchmark%s-%d\t%d\t%.0f ns/op\t%.0f B/op\n",
        $safe,
        $cpus,
        $iters,
        $r['ns_op'],
        $r['bytes_op']
    );
}

// ── Verify server is reachable ────────────────────────────────────────────────
$ch = curl_init("$baseUrl/health");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 5);
$body_response = curl_exec($ch);
$code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
@curl_close($ch);

if ($code !== 200 || trim($body_response) !== 'ok') {
    fwrite(STDERR, "[perf_client] ERROR: Server not reachable at $baseUrl/health (HTTP $code)\n");
    fwrite(STDERR, "Start it with: php bench/perf_server.php\n");
    exit(1);
}

// ── Scenario registry ─────────────────────────────────────────────────────────
$scenarios = [
    // --- Baseline ---
    'plaintext'               => ['GET',  "$baseUrl/plaintext",   ''],
    'health_check'            => ['GET',  "$baseUrl/health",      ''],

    // --- JSON responses (static payloads — no PHP work per request) ---
    'json_small'              => ['GET',  "$baseUrl/json/small",  ''],
    'json_1k'                 => ['GET',  "$baseUrl/json/1k",     ''],
    'json_10k'                => ['GET',  "$baseUrl/json/10k",    ''],
    'json_100k'               => ['GET',  "$baseUrl/json/100k",   ''],

    // --- JSON request body parse ---
    'echo_json_body'          => ['POST', "$baseUrl/echo/json",
                                  json_encode(['user' => 'test', 'value' => str_repeat('x', 200)])],

    // --- Route parameters ---
    'param_1_segment'         => ['GET',  "$baseUrl/users/42",              ''],
    'param_2_segments'        => ['GET',  "$baseUrl/users/42/posts/99",     ''],
    'param_3_segments'        => ['GET',  "$baseUrl/orgs/acme/repos/api/commits/deadbeef", ''],

    // --- Query string ---
    'query_string'            => ['GET',  "$baseUrl/search?q=hello&page=2&limit=50", ''],

    // --- Headers ---
    'header_read'             => ['GET',  "$baseUrl/headers/read", '',
                                  ['Authorization: Bearer eyJhbGciOiJIUzI1NiJ9',
                                   'Accept: application/json',
                                   'User-Agent: KislayBench/1.0']],
    'header_write_4x'         => ['GET',  "$baseUrl/headers/write", ''],

    // --- Request attributes ---
    'request_attributes'      => ['GET',  "$baseUrl/attrs", ''],

    // --- Middleware ---
    'middleware_with_use'     => ['GET',  "$baseUrl/mw/1",     ''],
    'middleware_plain'        => ['GET',  "$baseUrl/mw/plain", ''],

    // --- Route groups ---
    'group_plain'             => ['GET',  "$baseUrl/api/ping",            ''],
    'group_param'             => ['GET',  "$baseUrl/api/users/77/profile", ''],

    // --- Async / Promises ---
    'async_fire_and_forget'   => ['GET',  "$baseUrl/async/fire",  ''],
    'async_promise_chain'     => ['GET',  "$baseUrl/async/chain", ''],

    // --- File serving ---
    'file_1k'                 => ['GET',  "$baseUrl/file/1k",   ''],
    'file_10k'                => ['GET',  "$baseUrl/file/10k",  ''],
    'file_100k'               => ['GET',  "$baseUrl/file/100k", ''],

    // --- Status helpers ---
    'status_200_ok'           => ['GET',  "$baseUrl/status/200", ''],
    'status_400_bad_request'  => ['GET',  "$baseUrl/status/400", ''],
    'status_404_not_found'    => ['GET',  "$baseUrl/status/404", ''],
    'status_500_error'        => ['GET',  "$baseUrl/status/500", '', [], true],
];

// Sections define order and grouping
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

// ── Banner ────────────────────────────────────────────────────────────────────
echo "\n══════════════════════════════════════════════════════════════════════════════\n";
echo " KislayPHP Core – Performance Benchmark  (Go-bench style)\n";
echo "══════════════════════════════════════════════════════════════════════════════\n";
printf(" Server      : %s\n", $baseUrl);
printf(" Concurrency : %d\n", $concurrency);
if ($durationMs > 0) {
    printf(" Duration    : %d ms per scenario\n", $durationMs);
} else {
    printf(" Requests    : %d per scenario\n", $requests);
}
printf(" Runs        : %d%s\n", $runs, $runs > 1 ? " (CV stability check)" : "");
if ($only) {
    printf(" Filter      : %s\n", $only);
}
echo " Columns     : req/s | ns/op | p50 | p95 | p99 | p999 | bytes/op | errors | total\n";
echo "══════════════════════════════════════════════════════════════════════════════\n";

// ── Run scenarios ─────────────────────────────────────────────────────────────
$summary       = [];
$gobenchLines  = [];
$ncpus         = (int)(shell_exec('nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 1') ?: 1);

foreach ($sections as $sectionName => $keys) {
    $sectionResults = [];

    foreach ($keys as $key) {
        if ($only && $key !== $only) {
            continue;
        }
        if (!isset($scenarios[$key])) {
            continue;
        }

        [$method, $url, $bodyArg]   = $scenarios[$key];
        $extraHeaders = $scenarios[$key][3] ?? [];
        $allow5xx     = (bool)($scenarios[$key][4] ?? false);

        // Warmup — adaptive: more warmup for POST/file scenarios
        $warmupCount = str_starts_with($key, 'file_') ? 10 : 50;
        warmup($url, $warmupCount, $method, $bodyArg, $extraHeaders);

        // Measure
        $result = run_benchmark(
            $key, $url, $concurrency, $requests,
            $method, $bodyArg, $extraHeaders, $allow5xx,
            $durationMs, $runs, $cvWarnPct
        );
        $sectionResults[$key] = $result;
        $summary[$key]        = $result;
    }

    if (empty($sectionResults)) {
        continue;
    }

    print_section_header($sectionName);
    foreach ($sectionResults as $key => $result) {
        print_result("  $key", $result);
        if ($emitGoBench) {
            $gobenchLines[] = [$key, $result, $ncpus];
        }
    }
}

// ── Summary table — sorted by throughput ─────────────────────────────────────
if (count($summary) > 1) {
    echo "\n══════════════════════════════════════════════════════════════════════════════\n";
    echo " Summary — sorted by throughput (highest req/s first)\n";
    echo "══════════════════════════════════════════════════════════════════════════════\n";
    echo "  %-30s | %9s | %8s | %8s | %8s | %9s | %9s\n";
    printf(
        "  %-30s | %9s | %8s | %8s | %8s | %9s | %9s\n",
        'Scenario', 'req/s', 'ns/op', 'p50', 'p99', 'bytes/op', 'errors'
    );
    echo '  ' . str_repeat('-', 103) . "\n";

    uasort($summary, fn($a, $b) => $b['rps'] <=> $a['rps']);
    $rank = 1;
    foreach ($summary as $k => $r) {
        $errTag = $r['errors'] > 0 ? sprintf('!%d', $r['errors']) : '0';
        printf(
            "  #%-2d %-26s | %9.1f | %8.0f | %8s | %8s | %9s | %9s\n",
            $rank++,
            $k,
            $r['rps'],
            $r['ns_op'],
            fmt_lat($r['p50_ms']),
            fmt_lat($r['p99_ms']),
            fmt_bytes($r['bytes_op']),
            $errTag
        );
    }

    reset($summary);
    $fastest = key($summary);
    end($summary);
    $slowest = key($summary);
    echo "\n";
    printf("  Fastest: %-28s  %8.1f req/s  %8.0f ns/op\n", $fastest, $summary[$fastest]['rps'], $summary[$fastest]['ns_op']);
    printf("  Slowest: %-28s  %8.1f req/s  %8.0f ns/op\n", $slowest, $summary[$slowest]['rps'], $summary[$slowest]['ns_op']);

    // Ratio: how many times slower is the slowest vs fastest
    if ($summary[$slowest]['rps'] > 0) {
        $ratio = $summary[$fastest]['rps'] / $summary[$slowest]['rps'];
        printf("  Spread : %.1fx between fastest and slowest\n", $ratio);
    }
}

// ── Go-bench output ───────────────────────────────────────────────────────────
if ($emitGoBench && !empty($gobenchLines)) {
    echo "\n# Go-bench format (compatible with benchstat)\n";
    foreach ($gobenchLines as [$key, $result, $ncpu]) {
        emit_gobench_line($key, $result, $ncpu);
    }
}

// ── JSON output ───────────────────────────────────────────────────────────────
if ($emitJson) {
    echo "\n# JSON\n";
    echo json_encode([
        'meta' => [
            'server'      => $baseUrl,
            'concurrency' => $concurrency,
            'requests'    => $requests,
            'duration_ms' => $durationMs,
            'runs'        => $runs,
            'timestamp'   => date('c'),
        ],
        'results' => $summary,
    ], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";
}

echo "\n[perf_client] Done.\n";
