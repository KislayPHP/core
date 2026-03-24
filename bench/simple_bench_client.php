<?php

$url = $argv[1] ?? 'http://127.0.0.1:9090/plaintext';
$concurrency = (int) ($argv[2] ?? 10);
$totalRequests = (int) ($argv[3] ?? 1000);

echo "Benchmarking $url\n";
echo "Concurrency: $concurrency, Total Requests: $totalRequests\n";

$startTime = microtime(true);
$completed = 0;
$mh = curl_multi_init();
$handles = [];

function add_request($mh, $url, &$handles) {
    $ch = curl_init($url);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    curl_multi_add_handle($mh, $ch);
    $handles[(int)$ch] = $ch;
}

// Initial batch
for ($i = 0; $i < min($concurrency, $totalRequests); $i++) {
    add_request($mh, $url, $handles);
}

$active = null;
do {
    while (($status = curl_multi_exec($mh, $active)) == CURLM_CALL_MULTI_PERFORM);
    if ($status != CURLM_OK) break;

    while ($info = curl_multi_info_read($mh)) {
        $ch = $info['handle'];
        $completed++;
        curl_multi_remove_handle($mh, $ch);
        @curl_close($ch);
        unset($handles[(int)$ch]);

        if ($completed + count($handles) < $totalRequests) {
            add_request($mh, $url, $handles);
        }
    }

    if ($active) curl_multi_select($mh);

} while ($active || count($handles) > 0);

$endTime = microtime(true);
$duration = $endTime - $startTime;
$rps = $completed / $duration;

curl_multi_close($mh);

printf("\nResults:\n");
printf("Time taken: %.3f seconds\n", $duration);
printf("Completed requests: %d\n", $completed);
printf("Requests per second: %.2f\n", $rps);
printf("Average latency: %.3f ms\n", ($duration / $completed) * 1000 * $concurrency);
