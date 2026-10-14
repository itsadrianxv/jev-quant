# Running offline performance experiments

Read [metric definitions](performance-benchmark.md) before interpreting results.
The first use is a small validity trial. A formal baseline and evidence-driven
optimization are later work; the trial is not a performance claim.

## Delivery validation

On 2026-10-14, WSL Ubuntu with GCC 13.3 passed fresh Debug and Release builds,
290/290 CTest cases in each configuration, and a Release-profile build. The
commands below completed 23 native runs across all scenarios/modes and three
Callgrind runs, with 32 measured inputs, eight warmup inputs, seed 42 and one
repetition. Each profile contained the named measured-batch dump. This verifies
the workflow, not statistical stability or a performance baseline.

## Build and transfer

Follow the repository's Windows worktree workflow. Capture the source revision
before copying into the .git-free WSL build directory; supply that value as
`--revision`. For uncommitted experiments, append `-dirty` and retain the diff.
Use local staging followed by Copy-Item to the WSL UNC path, verify file hashes,
and run the following commands in the WSL copy. Do not robocopy directly to UNC.

```bash
cmake --preset local-gcc-release-profile
cmake --build --preset local-gcc-release-profile --parallel 3
```

The preset retains Release optimization and adds debug symbols and frame pointers.
The runner requires Linux, Python 3.11+, `taskset` and `lscpu`. Callgrind requires
Valgrind and its headers installed before compilation. Ordinary native builds
work without the headers; requesting an unavailable profile fails explicitly.

## Validate the workflow

Set `REV` to the captured source identity and choose a new output directory:

```bash
REV=<captured-source-revision>
python3 scripts/profiling/run_benchmarks.py \
  --bench build/Release-profile/jev_bench --revision "$REV" \
  --updates 32 --warmup 8 --repetitions 1 --out tmp/bench-trial

python3 scripts/profiling/run_benchmarks.py \
  --bench build/Release-profile/jev_bench --revision "$REV" \
  --scenarios snapshot-5 incremental-32 pipeline-fill --modes callgrind \
  --updates 32 --warmup 8 --repetitions 1 --out tmp/callgrind-trial
```

The native matrix covers all ten scenarios, throughput and latency, and the
three pipeline stage runs. Each run validates its expected work and output.
CTest's `benchmark_contract` independently recomputes percentiles from raw samples,
checks mode equivalence and deliberate failure cases. Component tests verify
observer placement, early exits and individual response observations.

## Conduct a study

1. Choose a question, such as how incremental book population affects update
   cost, or whether fills cost more than acceptance processing. Keep scenario
   differences explicit. Comparing HOLD and fill cycles does not isolate a
   single function because they execute different business work.
2. Choose fixed seed, warmup, measured count and CPU. `--cpu N` must name an
   allowed CPU; by default the runner chooses the last allowed one and records it.
   Start with 100,000 measured inputs, 1,000 warmup inputs and five repetitions.
   Increase count if a batch is too short or percentile variability is large.
   Each count is limited to 1,000,000 to bound memory. Use multiples of eight
   for a full matrix so all scenario periods align.
3. Run native throughput and latency separately from stage timing and profiling.
   Keep other host activity low and record changes in power/thermal conditions.
4. Use Callgrind on a smaller fixed load to attribute instruction cost. Keep
   that load identical across versions. Compare inclusive call paths and self
   costs; inclusive percentages overlap and must not be summed.
5. Inspect raw samples, work counts and result checksums before interpreting
   differences. Compare repetitions and observation controls. Form a hypothesis
   for a later change only after the measurements support it.

```bash
python3 scripts/profiling/run_benchmarks.py \
  --bench build/Release-profile/jev_bench --revision "$REV" \
  --scenarios incremental-32 incremental-512 incremental-8192 \
  --modes throughput latency --updates 100000 --warmup 1000 \
  --repetitions 5 --out tmp/book-study

python3 scripts/profiling/run_benchmarks.py \
  --bench build/Release-profile/jev_bench --revision "$REV" \
  --scenarios pipeline-hold pipeline-cancel pipeline-fill --modes stages \
  --updates 100000 --warmup 1000 --repetitions 5 --out tmp/stage-study
```

`scripts/profiling/generate_baselines.sh` is a convenience wrapper around the
runner, not a command that overwrites the historical committed baseline.

## Evidence and failure interpretation

Every experiment contains `manifest.json`, per-run command/stdout/stderr, and
`data/summary.json`. Timing runs also contain `data/samples.csv`. Callgrind runs
retain raw output, the named `measured-batch` dump, annotation and `profile.json`.
`runs.tsv` lists individual metrics; `comparison.tsv` reports run medians and
ranges, including the median of per-run p99 values rather than a pooled p99.
Only a fully successful experiment has a `COMPLETE` file.

The manifest records source revision, binary SHA-256, CMake flags, kernel/WSL
environment, CPU affinity, tool versions and arguments. Summaries record compiler,
seed, counts and a deterministic 64-bit workload fingerprint. That fingerprint
is an integer-mixing checksum, not a cryptographic hash or a market-data archive.
Save the source snapshot/diff with the artifacts if the revision is dirty.
Archive results outside the disposable WSL build directory before deleting it.

An occupied output directory is rejected. Nonzero exit, timeout, count mismatch,
sample overflow or missing measured Callgrind dump invalidates the experiment;
inspect that run's stderr. Do not average its partial artifacts with good runs.
The `--sample-capacity` bench option allows testing overflow behavior. It is not
a sampling-rate control. Old log percentiles are not a fallback for lost samples.

## Optional tools and later experiments

`perf stat -e cycles -- true` tests PMU availability. If available, use perf on a
native throughput run to inspect CPU samples; otherwise test `cpu-clock` support.
A WSL perf wrapper can mismatch the kernel: use the installed versioned binary
only after checking it works. Record the event, frequency and tool version.
Whole-process perf samples include preflight and setup, unlike the scoped
Callgrind numbers; attribute them accordingly. FlameGraph visualization is optional.
Massif is optional heap investigation, not proof of an allocation-free hot path.

TODO: Design asynchronous arrival-rate, queueing, overload and real venue studies
separately. This version does not measure those effects, simulate market event
frequencies, introduce TSC timing, or gate CI on performance numbers.
