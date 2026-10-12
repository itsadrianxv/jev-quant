# TTT profiling methodology

How to measure and profile the participant pipeline deterministically on this
project's development environment, and how to compare runs against the
committed baseline artifacts in `docs/profiling/baseline/`.

## Environment limitations (read first)

The profiling environment is a laptop WSL2 image. Turbo Boost, core
isolation, hyperthreading, and ASLR cannot be controlled from inside WSL2.
Consequences:

- **Absolute numbers are only comparable within this environment.** Never
  compare them against numbers from another machine, another kernel, or
  bare-metal Linux.
- Run-to-run wall-clock variance is real. Compare percentile *distributions*
  and relative segment shares across repeated runs on the same machine, and
  prefer the deterministic counters from callgrind when a precise ratio is
  needed.
- Every reported run must state the binary revision (`git rev-parse HEAD`)
  and the exact load (`--updates` value).

Hardware counters: older WSL2 kernels expose no PMU at all, and the
committed recipes therefore default to the `cpu-clock` software event so
they behave identically everywhere. This particular image runs kernel
`6.18.40.1-microsoft`, which does expose a guest PMU (`cycles:u` counts);
using `-e cycles` instead of `-e cpu-clock` is fine here and gives cleaner
call-graph sampling, but results must not be treated as portable across
kernels either way.

Note the distro `perf` wrapper refuses to run when its version does not
match the running kernel (`WARNING: perf not found for kernel
6.18.40.1-microsoft`). Call the versioned binary directly instead:

```bash
PERF=/usr/lib/linux-tools/6.8.0-146-generic/perf
```

## Prerequisites

Inside WSL Ubuntu:

```bash
sudo apt-get install linux-tools-generic valgrind python3
```

- `perf` comes from `linux-tools-generic` (version-matched wrapper; the WSL2
  kernel has no hardware PMU, so only software events work, see below).
- `valgrind` provides callgrind and massif.
- `python3` runs the percentile analysis script.

## Build the profiling binary

The committed CI presets stay untouched. Profiling uses the local preset,
which is Release-optimized but keeps debug symbols and frame pointers so
stacks unwind correctly:

```bash
cmake --preset local-gcc-release-profile
cmake --build --preset local-gcc-release-profile
```

The bench binary is `build/Release-profile/jev_bench`. It drives the full
participant pipeline in-process — TradeEngine, OrderGateway, JevWorker with
the deterministic `MockJevDecisionProvider`, and a synthetic venue that
accepts new orders and honors cancels without filling — over a fixed load of
synthetic depth snapshots. There is no network access. Load size is fixed by
default (`--updates 100000`); override it with `--updates N`. Logs go to
`--out DIR` (default `<tmp>/jev_quant_bench_ttt`). At exit the bench parses
the TTT producer log of the run and prints a per-segment
count/mean/p50/p90/p99/max table, so the printed summary is exactly the set
of records written during the run.

The TTT record format (one line per consumed event):

```
<epoch_us> [LEVEL] [TTT] event=<kind> evaluation_id=<n> ticker_id=<n> elapsed_ns=<n>
```

Latency segments: `ttt_market_data_consumed` (venue publish to TradeEngine
consume), `ttt_decision_consumed` (JevWorker enqueue to TradeEngine consume),
`ttt_decision_to_order` (decision handling start to client request enqueue),
`ttt_order_round_trip` (OrderGateway response publish to TradeEngine
consume). Drop records (`ttt_decision_stale_drop`,
`ttt_decision_blocked_*`) carry no `elapsed_ns` and are counted separately.
The synthetic load never trips the drop paths.

## Percentile analysis

```bash
python3 scripts/ttt_percentiles.py <out>/ttt-1.log
```

Standard library only. Percentiles are nearest-rank over ascending-sorted
samples (`index = max(1, ceil(q*n)) - 1`). The script is verified against the
committed fixture `scripts/testdata/ttt-fixture.log` (CTest entry
`ttt_percentiles_fixture` runs it with `--check`).

## CPU pinning

Pin the bench to one fixed core so the profile and the latencies are not
smeared across cores:

```bash
taskset -c "$(($(nproc) - 1))" build/Release-profile/jev_bench --out /tmp/ttt-run
```

WSL2 does not expose isolation controls, so pinning is the only scheduling
control available. Keep the pinned core identical between runs you compare.

## perf record / report (primary tool)

Check what the running kernel supports before choosing the event:

```bash
perf stat -e cycles -- true   # counts: guest PMU available
                              # "not supported": use the software event
```

The committed fallback is the `cpu-clock` software event; on a kernel with a
guest PMU, `-e cycles` is the better default:

```bash
taskset -c 7 $PERF record -F 4999 -g -e cpu-clock -o perf.data \
    -- build/Release-profile/jev_bench --out /tmp/ttt-perf
$PERF report -i perf.data
$PERF script -i perf.data > perf.script
```

## FlameGraph (pinned upstream revision)

Clone the toolchain **outside the repository** at the pinned revision and
keep that clone for every future graph so the rendering stays comparable:

```bash
git clone https://github.com/brendangregg/FlameGraph ~/tools/FlameGraph
git -C ~/tools/FlameGraph checkout 41fee1f99f9276008b7cd112fca19dc3ea84ac32

$PERF script -i perf.data \
    | ~/tools/FlameGraph/stackcollapse-perf.pl \
    | ~/tools/FlameGraph/flamegraph.pl > flamegraph.svg
```

## callgrind with branch simulation (verification tool)

Instruction and branch counts are deterministic and machine-independent, so
they are the tie-breaker when sampling results are ambiguous. Expect a ~50x
slowdown; use a reduced load:

```bash
valgrind --tool=callgrind --branch-sim=yes \
    --callgrind-out-file=callgrind.out \
    build/Release-profile/jev_bench --updates 20000 --out /tmp/ttt-cg
callgrind_annotate --threshold=99 callgrind.out
```

## massif (heap verification)

Verifies the zero-allocation claim on the hot path: the heap must reach a
plateau after startup and stay flat while the bench runs.

```bash
valgrind --tool=massif --massif-out-file=massif.out \
    build/Release-profile/jev_bench --updates 20000 --out /tmp/ttt-ms
ms_print massif.out
```

## Baseline artifacts and regeneration

Committed baseline (unmodified pipeline, current binary at the recorded
revision):

- `docs/profiling/baseline/flamegraph.svg` — perf cpu-clock flame graph.
- `docs/profiling/baseline/callgrind-annotate.txt` — callgrind annotation.
- `docs/profiling/baseline/massif.txt` — heap plateau over the run (the
  zero-allocation verification).
- `docs/profiling/baseline/ttt-percentiles.tsv` — per-segment percentile
  table from the bench's own TTT log.
- `docs/profiling/baseline/ttt-bench-summary.txt` — the bench's exit summary
  plus the revision and environment annotations.

Regenerate everything with one command (build first, set `FLAMEGRAPH_DIR` if
the FlameGraph clone lives elsewhere):

```bash
scripts/profiling/generate_baselines.sh
```

Post-optimization runs (issue 03) regenerate the same artifacts and diff the
percentile tables and flame graphs against this baseline.
