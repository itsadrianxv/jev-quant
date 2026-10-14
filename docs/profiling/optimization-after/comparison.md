# Historical optimization: before/after

These results use the previous whole-process profiling and lossy-log timing protocol. They are not comparable with the current SimNow measurement protocol. See [the current methodology](../performance-benchmark.md).

The committed PR 2 callgrind baseline identified `MarketOrderBook::clearIncrementalBook()` and the STL array fill beneath it as 98.98% of instructions (15,238,140,000); the depth-snapshot bench never has incremental entries after entering depth mode, so repeatedly clearing the 64K order and 4K-per-side slot arrays was unnecessary. The optimization skips that clear for `UNINITIALIZED` and `DEPTH_SNAPSHOT` books while rejecting mixed incremental/snapshot use. The current implementation does not transition an incremental book into snapshot mode.

## Callgrind

| Run | Instructions | Branches | Command |
| --- | ---: | ---: | --- |
| Before | 15,395,670,620 | 4,136,268,469 | `jev_bench --updates 20000` |
| After | 146,063,371 | 38,605,484 | `jev_bench --updates 20000` |
| Change | -99.05% | -99.07% | same Release-profile bench and WSL2 environment |

The after annotation shows `clearIncrementalBook` absent from the top 99% of instructions. `flamegraph.svg`, `callgrind-annotate.txt`, and `massif.txt` were generated from the after run. The baseline flame graph and annotation remain under `docs/profiling/baseline/`.

## TTT percentiles

The same default bench command (`--updates 100000`, pinned CPU, Release-profile) produced the following records. The faster after run overran the fixed logger queue, so counts are included explicitly; percentile values are computed only from records actually written.

| Segment | Before count / p50 / p90 / p99 (ns) | After count / p50 / p90 / p99 (ns) |
| --- | ---: | ---: |
| decision consumed | 100000 / 30 / 31 / 81 | 59564 / 30 / 40 / 90 |
| decision to order | 100000 / 50 / 70 / 210 | 59563 / 40 / 50 / 140 |
| market data consumed | 100000 / 21 / 30 / 50 | 59559 / 30 / 31 / 150 |
| order round trip | 100000 / 20 / 40 / 171 | 59559 / 30 / 31 / 110 |

The count difference is a measurement artifact from the existing bounded logger queue under the much faster optimized run; it does not alter the pipeline behavior or the fixed-load callgrind comparison.
