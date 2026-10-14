# Offline performance benchmark

`jev_bench` measures the participant-side C++ core with deterministic synthetic
inputs. It answers how much work book maintenance and a synchronous trading cycle
require, and which processing stages account for that work. It does not establish
production HFT latency, live tick-to-trade, venue round-trip time, or system capacity
under an external arrival rate. See [the experiment playbook](performance-experiments.md)
for commands and artifacts.

## Workloads

| Scenario | Work performed | Why measure it? |
| --- | --- | --- |
| `snapshot-1`, `snapshot-5` | Replace a normalized book snapshot, with 1 or 5 populated levels per side | Cost of accepting a fresh book view |
| `snapshot-boundary` | Valid, one-sided, empty, restored snapshot | Keep missing-liquidity paths visible |
| `incremental-32/512/8192` | Seeded MODIFY/CANCEL/ADD/MODIFY blocks at the named resting-order population | Sensitivity to book size, lookup and level maintenance |
| `incremental-boundary` | CLEAR, rebuild, remove the last order on a side, restore, modify | Separate reset and best-level removal costs |
| `pipeline-hold` | Market update, features, evaluation, HOLD | State maintenance without order emission |
| `pipeline-cancel` | Alternate NEW/ACCEPTED and CANCEL/CANCELED cycles | Order lifecycle and repricing cost |
| `pipeline-fill` | Alternate open and close, each with ACCEPTED and full FILLED | Position, close allocation and PnL accounting cost |

The measured book is `Trading::MarketOrderBook`, after venue normalization. Binance
JSON parsing, snapshot/diff synchronization, transport and its adapter-side depth
book are excluded. Snapshot storage always has five slots per side; populated
depth is not a configurable storage capacity. Incremental populations have 4,
64 and 1024 price levels per side, with four orders per level. Each four-event
block has two MODIFY, one CANCEL and one ADD; occupancy drops by one temporarily.
The boundary sequence has eight events including one CLEAR. These are controlled
scenarios, not estimates of market event frequencies or near-capacity stress.

A synchronous cycle publishes one pre-generated valid snapshot, updates the real
book/features, constructs the real evaluation state, registers one evaluation,
runs a scripted provider through JevWorker, handles the decision, drives
OrderGateway and consumes all prescribed responses. There are no worker threads
or network requests. Automatic evaluation scheduling is not attached; input
sequence rather than wall time controls evaluations. Queues are empty at each
cycle boundary. Feature, risk, order and position code is shared with the app.

Each HOLD cycle has zero requests/responses. A cancel pair has one NEW and one
CANCEL, with one response per cycle. A fill pair has two NEW requests and four
responses, ending flat with no live orders. Quantity is one; fixed bid/ask 100/101
produces realized PnL of -1 per fill pair, before fees. Close uses CLOSE_TODAY.
Order-size and position limits are one; the existing zero loss-limit setting
disables cumulative-loss rejection so repetitions retain the same work. The
single-order venue gate is not enabled. This is not coverage of every risk rule.

## Metrics and boundaries

| Mode / metric | Included work | Interpretation |
| --- | --- | --- |
| `throughput`: operations/s, batch ns/operation | Measured batch including driver, result consumption and business counters | Fixed-work throughput; ns/operation is an average |
| `latency`: book event duration | One book update call | Distribution of update processing costs, grouped by event type |
| `latency`: cycle duration | Entire synchronous cycle, including queues, scripted provider and simulated venue | Controlled whole-cycle cost, not real execution latency |
| `stages`: market duration | Book update plus feature and position market-state updates | Local work before an updated state is available |
| `stages`: decision duration | Full decision handler, including early checks and risk/order actions | Decision processing, including no-request outcomes |
| `stages`: response duration | Full response handler including order, position and PnL changes | Cost of acceptance, cancellation or fill processing |
| `callgrind`: Ir, Ir/operation | Same fixed batch as throughput, collected under Valgrind | Executed instruction attribution and cost per input |

Stage intervals include the existing business callbacks. They exclude queue-drain
bookkeeping, preceding text logging, and the observer's storage callback. A null
observer adds no stage clock reads. Existing application timestamp reads remain
part of the workload. Whole-cycle and stage timings run in different processes
with the same inputs. Stage p99 values cannot be added to obtain cycle p99.
Decision labels include intent and actual emitted request type, or `no_request`.
`no_request` can include HOLD, stale decisions, risk rejection or a pending order;
it does not identify a unique rejection reason. Response types remain separate.

Durations use `steady_clock`, explicitly converted to nanoseconds. Report sample
count, mean, nearest-rank p50/p90/p99 and maximum. For sorted N samples the rank
is `ceil(q*N)`, starting at one. Maximum is an observation, not a latency bound.
Small trial p99 values have little statistical value. WSL2 scheduling, host load
and power management affect timing even with CPU affinity.

## Measurement validity

Inputs and sample storage are allocated before timing. Every input is first
checked in an untimed preflight: an independent ordered-map model verifies book
BBO after each update; snapshot checks compare all five levels and metadata;
pipeline checks verify intermediate BBO, positions, order
states, offsets and known PnL. A fresh measured instance then warms up. Measured
results feed a rolling checksum after each operation, including full depth and
metadata for snapshot components; that consumption cost is
included in throughput and Callgrind, outside individual latency intervals.
Pipeline business counters and request-field checks are included in cycle cost.
Warmup and measurement counts align to scenario periods; state and IDs continue
across warmup, but measured counts exclude it.

The benchmark uses preallocated numeric samples, not the bounded text logger.
Overflow, missing samples, unexpected work counts, queue residue or wrong output
fails the run. Samples are written only afterward. An empty clock-pair control
and clock-pair-plus-record batch estimate expose observation cost separately;
these are controls, not corrections subtracted from percentiles. Stage record
cost can affect later events even though it lies outside each stage interval.

Callgrind collects only the measured batch; initialization, preflight, warmup,
sorting and output are excluded. Counts depend on the binary, libraries,
workload and runtime path. Branch simulation is optional model output, not
hardware branch-miss measurement. Never use Valgrind wall time as native latency.
Massif can describe live heap size but a plateau does not prove zero allocations.

The experiment runner uses fresh processes, records build/environment/input
identity, and checks business-result equivalence across modes and repetitions.
It reports individual runs and median/min/max across runs; it does not pool
latency samples from different runs or enforce timing thresholds in CI.

## Existing TTT observations

Old `ttt_order_round_trip` records response-publication-to-consumption time, not
request-to-response RTT. Old `ttt_decision_to_order` records part of decision
handling and can include no-request paths. These live event names are retained;
the new benchmark does not derive metrics from them. Historical whole-process
Callgrind totals and lossy-log percentiles under `baseline/` and
`optimization-after/` are not directly comparable with this protocol.
