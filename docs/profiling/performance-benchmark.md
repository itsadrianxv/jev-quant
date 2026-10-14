# SimNow performance benchmark

## Scope

Measure the existing SimNow application with the real Jev HTTP provider. There is no standalone synthetic benchmark. Native runs measure elapsed time; separate Callgrind runs explain engine instruction hotspots. Neither measures production-exchange latency or maximum sustainable throughput.

## Eight timestamps

All timestamps use the same process-local steady clock, expressed in nanoseconds. Zero means absent. The requested Chinese definitions are retained here and in the experiment guide.

| 时间戳 | 定义 |
| --- | --- |
| t1 | 进入 CTP 行情回调；不是 NIC 收包时间，也不包含 SDK 此前的接收处理。 |
| t2 | MarketDataConsumer 所在线程从适配器行情事件队列取得该事件。 |
| t3 | MarketDataConsumer 即将把标准化行情发布到交易引擎输入队列。 |
| t4 | 交易引擎从行情输入队列取得该条标准化行情。 |
| t5 | 交易引擎即将把实际订单请求发布到 OrderGateway 输入队列。 |
| t6 | OrderGateway 从订单请求队列取得该请求。 |
| t7 | OrderGateway 即将调用适配器提交接口，将请求交给适配器。 |
| t8 | 适配器完成参数准备，即将调用 CTP 下单接口；不是 NIC 发包时间。撤单单独使用 CTP 撤单接口前的时间。 |

The queue publication stamps precede the release that makes the event visible. Queue intervals therefore include the tail of publication, not just residence in queue. t7 is a synchronous module boundary, not another queue.

| Interval | Interpretation |
| --- | --- |
| t2-t1 | Callback work and adapter market-event queue waiting |
| t3-t2 | Market normalization and publication preparation |
| t4-t3 | Engine market-input queue waiting, including publication tail |
| t5-t4 | Evaluated market state to request publication, including asynchronous evaluation scheduling, provider HTTP work, decision waiting and local processing |
| t6-t5 | Order-request queue waiting, including publication tail |
| t7-t6 | Gateway processing before adapter handoff |
| t8-t7 | Adapter and transport argument preparation before the CTP API call |
| t8-t1 | Local callback-to-submission-call elapsed time for the evaluated source state |

Compute total elapsed time directly for each row. Never add stage percentiles to produce a total percentile. None of these intervals is an exchange acknowledgement RTT.

## Provenance and sample population

The engine remembers the last consumed market event per instrument. An evaluation takes that source by value. JevWorker drains to the newest queued evaluation and copies that selected evaluation's metadata into its decision; provider-returned metadata cannot replace it. The decision carries the source into each NEW request it produces. A mismatched provider evaluation or instrument identity receives no source correlation.

The source identifies the evaluated state, not the sole cause of the order. Evaluation also uses features, account and position state. Execution pricing still uses the current book when the decision is handled, which can be newer than the evaluated quote. Multiple evaluations can share one source. Do not treat observations as independent market ticks.

The main population contains only NEW requests with all eight ordered timestamps, nonzero source/evaluation IDs, and an immediate CTP return code of zero. Report OPEN and CLOSE_TODAY separately. Code zero is local submission success, not exchange acceptance. HOLD, stale decisions, risk rejection, adapter rejection and immediate API failure do not enter the main report. Existing business logs describe them; there is no new performance outcome taxonomy. This successful-submission selection is biased and cannot describe all input events.

CANCEL requests have only t5-t8 and are reported separately. Shutdown-generated adapter cancellations bypass the engine queue and are not samples. No response-processing metrics, provider-duration metrics, NIC timestamps or order-confirmation correlation are collected.

## Collection and validity

Capture is opt-in and SimNow-only. Main arms it after venue readiness. CTP callbacks admit new sources for window_seconds. Evaluations built after that window carry no observation metadata; previously queued/selected evaluations can finish during grace_seconds. Recording accepts submissions whose t8 is before the grace deadline. Admission stopping never disables normal evaluations or trading. Late and incomplete chains are omitted, so finite grace still censors slow chains.

Samples use preallocated storage written only by the gateway thread. The market callback owns source numbering; the engine owns source selection. Metadata crosses existing SPSC queues by value. Export happens after workers join, outside measured intervals. Capacity exhaustion increments overflow without changing order execution; any overflow invalidates the statistical report. Files are never silently overwritten. Normal shutdown before the deadline is marked incomplete. Fatal process termination may leave no capture files; retain console/run evidence and do not treat it as a successful experiment.

The summary reports n, min, max and nearest-rank p50/p95/p99 in nanoseconds by request type and offset. Counts below 1000 carry a tail-sample warning, not a guarantee that 1000 samples suffice. Provider failures, venue health and window completion are recorded. No complete NEW samples means live acceptance remains pending.

Clock reads, metadata copies and the enabled collector have overhead. The disabled path avoids observation clock reads, but enlarged internal queue messages still occupy memory. Keep the same logging settings across native runs; business callbacks and logging inside processing remain part of the workload. Do not subtract an estimated clock cost from percentiles or claim nanosecond clock resolution equals measurement accuracy.

## Callgrind

Run the same application under Callgrind with --collect-atstart=no and --separate-threads=yes. Instrumentation stays enabled; collection toggles only on the engine thread around book plus market handling and around decision handling. RAII restores collection on early returns or exceptions. Queue polling, response handling, gateway, adapter, HTTP worker and report output are outside these scopes. Tiny scope-control costs remain in the profile.

The collection window uses scope entry time, so a scope started before its end can finish afterward. Market and decision counts cover all collected scopes, including HOLD and rejected decisions; these counts differ from complete order samples. Inspect the engine thread's inclusive/self instruction costs and call tree. Do not divide combined profile instructions by successful orders, infer separate per-stage costs from one mixed total, or interpret Callgrind wall time as native latency.

Valgrind slows the whole process even while collection is off. It can change provider timeouts, queue occupancy and decision staleness. SDK compatibility and live capture validity require actual SimNow evidence. The local callgrind_scope_test verifies collection exclusion across threads; it does not certify live SDK compatibility.

References: https://valgrind.org/docs/manual/cl-manual.html#cl-manual.limits and https://valgrind.org/docs/manual/cl-manual.html#cl-manual.clientrequests .

## Delivery status

Implementation and local checks are separate from live acceptance. Real SimNow native and Callgrind trials are pending a trading session. No live latency baseline or optimization claim is established by this change. Archived baseline/ and optimization-after/ artifacts belong to retired synthetic protocols and are not comparable to this protocol.
