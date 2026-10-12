#!/usr/bin/env bash
# Regenerate the committed baseline profiling artifacts under
# docs/profiling/baseline/.  Run from the repository root after building the
# local-gcc-release-profile preset.  Requires perf, valgrind, python3, and a
# FlameGraph checkout at the pinned revision (cloned outside the repository;
# cloned automatically when missing).  Transient run logs stay in a temp
# directory; only the reviewed artifacts are written into the tree.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BENCH="${BENCH:-build/Release-profile/jev_bench}"
OUT="docs/profiling/baseline"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/tools/FlameGraph}"
PINNED_FLAMEGRAPH_REV="41fee1f99f9276008b7cd112fca19dc3ea84ac32"
# The distro perf wrapper refuses to run when its version does not match the
# WSL2 kernel; point PERF at the versioned binary in that case, e.g.
# PERF=/usr/lib/linux-tools/6.8.0-146-generic/perf
PERF="${PERF:-perf}"

PIN_CPU="${PIN_CPU:-$(($(nproc) - 1))}"

if [ ! -x "$BENCH" ]; then
    echo "error: $BENCH is missing; run cmake --preset local-gcc-release-profile && cmake --build --preset local-gcc-release-profile" >&2
    exit 1
fi

mkdir -p "$OUT"
echo "revision: ${REV:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"

# Baseline run: pinned CPU, default fixed load, summary straight from the
# bench exit output and the percentile table from its TTT log.
taskset -c "$PIN_CPU" "$BENCH" --out "$WORK/run-ttt" > "$OUT/ttt-bench-summary.txt"
python3 scripts/ttt_percentiles.py "$WORK/run-ttt/ttt-1.log" > "$OUT/ttt-percentiles.tsv"

# Flame graph at the pinned upstream revision, recorded on the cpu-clock
# software event because the WSL2 kernel exposes no hardware counters.
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    git clone https://github.com/brendangregg/FlameGraph "$FLAMEGRAPH_DIR"
fi
git -C "$FLAMEGRAPH_DIR" checkout --quiet "$PINNED_FLAMEGRAPH_REV"
taskset -c "$PIN_CPU" "$PERF" record -F 4999 -g -e cpu-clock \
    -o "$WORK/perf.data" -- "$BENCH" --out "$WORK/perf-ttt" > /dev/null 2>&1
"$PERF" script -i "$WORK/perf.data" \
    | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
    | "$FLAMEGRAPH_DIR/flamegraph.pl" > "$OUT/flamegraph.svg"

# callgrind with branch simulation: deterministic instruction counts.
valgrind --tool=callgrind --branch-sim=yes \
    --callgrind-out-file="$WORK/callgrind.out" \
    "$BENCH" --updates 20000 --out "$WORK/callgrind-ttt" > /dev/null 2>&1
callgrind_annotate --threshold=99 "$WORK/callgrind.out" > "$OUT/callgrind-annotate.txt"

# massif: the hot path must not allocate (heap plateau after startup).
valgrind --tool=massif --massif-out-file="$WORK/massif.out" \
    "$BENCH" --updates 20000 --out "$WORK/massif-ttt" > /dev/null 2>&1
ms_print "$WORK/massif.out" > "$OUT/massif.txt"

echo "baseline artifacts written under $OUT"
