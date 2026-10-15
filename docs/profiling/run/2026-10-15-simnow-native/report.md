# SimNow native measurement: 2026-10-15

## Result

The real SimNow and Jev path completed one opening trade and one closing trade in rb2611. Both one-contract orders received FILLED responses. The application exited with code 0, position=0 and unresolved=0; no trading process remained.

Across the full configured 430-second runtime, 86 Jev decisions were observed: 84 HOLD, one OPEN (evaluation 40), and one CLOSE (evaluation 41). Provider failures were zero. These counts cover the whole runtime, not only the 300-second sample admission window. The preceding diagnostic run's seven HOLD decisions are excluded.

Two complete NEW submission traces were captured: one OPEN and one CLOSE_TODAY. Capture health and window-completion checks passed; overflow was zero. The original summary script validated both traces. This demonstrates live order-path and measurement functionality, not a statistically meaningful latency baseline.

## Conditions and provenance

- Source revision: `e5b0f983bcd8bb5512b796b922b27753c68605e7`, including PR #36's exact-contract response filter.
- Executable SHA-256: `0e471016af76c0e56dc7d28effa3dfe2b15e81baa4d3daeab7107d24dfce04db`.
- Input configuration SHA-256: `14b122f4cbb15b4dadc47f396195f6ba02adeaa7788e047e71e49ee761bc03b1`.
- Build: `local-gcc-release-profile`, Release with debug symbols and frame pointers, rebuilt before execution with one build job. Exact compiler version and host load were not recorded.
- Host: Linux 6.18.54.1-microsoft-standard-WSL2, x86_64, glibc 2.39. Recorded process affinity allowed CPUs 0-15; this does not imply dedicated cores or per-thread pinning.
- Venue: SimNow, rb2611; trading front `182.254.243.31:30001`, market front `182.254.243.31:30011`.
- Provider: OpenRouter Decisions API, `typesafe/jev-1.13`; timeout 4000 ms, zero retries. The user changed VPN region before this run after an earlier HTTP 403 regional-availability failure. Exact VPN region and routing were not recorded.
- Evaluation interval: 5000 ms; order quantity: one; queue capacity: 4096.
- Risk configuration: max_order_size=1, max_position=1, max_loss=0.
- Native capture: admission window 300 seconds, grace 120 seconds, capacity 10000. A temporary configuration set runtime.run_seconds=430; the original configuration was preserved.
- Verbose market-data logging was disabled; normal application logging remained enabled.
- Manifest UTC bounds: `2026-10-15T02:58:14.815879+00:00` to `2026-10-15T03:05:13.006045+00:00`.

The manifest wall-clock bounds span approximately 418.19 seconds, while capture steady-clock start/end span approximately 430.04 seconds (3657966881768 to 4088005450744 ns). The cause of this discrepancy was not investigated. Capture completion and latency use the monotonic steady clock; do not infer measurement duration from the UTC bounds.

## Observed durations

These are individual observations, not reliable estimates of latency percentiles.

| Interval | OPEN | CLOSE_TODAY |
| --- | ---: | ---: |
| t2-t1 | 4.461 us | 3.889 us |
| t3-t2 | 7.789 us | 5.117 us |
| t4-t3 | 0.250 us | 0.291 us |
| t5-t4 | 2893.879270 ms | 2214.881301 ms |
| t6-t5 | 0.271 us | 0.635 us |
| t7-t6 | 1.206 us | 0.832 us |
| t8-t7 | 8.112 us | 7.686 us |
| t8-t1 | 2893.901359 ms | 2214.899751 ms |

See [benchmark definitions](../../performance-benchmark.md) for timestamp boundaries and source association, and [experiment instructions](../../performance-experiments.md) for the procedure.

The t1 boundary is CTP market callback entry; t8 is immediately before invoking the CTP submission API. Neither is a NIC timestamp. The t4-t5 interval includes asynchronous decision waiting and provider HTTP work as well as processing; it is not pure CPU execution time. Exchange acknowledgement and fill latency are outside these measurements.

## Files and limitations

- `samples.csv` contains the two traces, with process-local order/source/evaluation identifiers and monotonic timestamps. It contains no account IDs, credentials or HTTP payloads.
- `summary.json` uses the original summary schema and nanosecond units. Each group has n=1, so min/max/P50/P95/P99 all equal its only observation. All metrics carry tail_sample_warning=true. These percentile fields must not be presented as reliable tail-latency measurements.

The sample population is complete submissions with immediate CTP return code zero. Separate business logs inspected during the run established that both orders subsequently filled. Immediate return code zero alone would not establish exchange acceptance or a fill.

HOLD evaluations and events without complete submissions are absent from the CSV. Decision/trade counts do not measure maximum throughput. No controlled optimization comparison or Callgrind run is included.

## Recovery and verification

The earlier report directory and original `results/simnow-native-vpn-20261015/` artifacts were unavailable when these files were restored. The CSV was reconstructed from the complete two-row CSV previously printed and inspected in this session. The summary was recomputed from those restored timestamps using the same per-group, single-sample statistics as the original summarizer. Both total durations were checked against the previously recorded summary.

Experiment conditions, decision counts, fills and capture-health results above are based on the manifest, capture metadata and business logs inspected before deletion. They cannot now be independently rechecked against the missing original files. The historical executable and configuration hashes are retained as provenance, not as newly verified hashes. No new live experiment was run during restoration, and no synthetic samples were added.

The restored CSV and summary were parsed and checked for complete, positive, nondecreasing timestamps, exactly two groups, and matching interval arithmetic. Full capture-health validation cannot be rerun without the original capture.json.

Restored artifact SHA-256 hashes:

- `samples.csv`: `5358c3114d59f5b775f5eb5df2d625ab9b744f8a4d739582973db18de6be7afe`
- `summary.json`: `24594e8000cd42db59814b87a70de403247aaa4086edd48333ced6a125b60a36`
