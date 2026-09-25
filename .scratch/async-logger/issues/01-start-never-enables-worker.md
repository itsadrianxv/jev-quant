# 01: AsyncLogger::start() never enables the worker loop

Status: resolved

## Problem

`AsyncLogger::start()` set `started_` and spawned the worker thread but never
stored `true` into `running_`. The worker main loop
`while (running_.load(std::memory_order_acquire))` was therefore false from
the first iteration: the polling loop was skipped entirely and the worker ran
only the one-shot shutdown drain before exiting. Messages logged after the
worker exited were never written to the producer files, and `stop()` joined an
already-finished thread.

## Evidence

GitHub Actions `C++ CI` failed on `tests/contract_tests.cpp:121` in
`testAsyncLoggerQueueOverflowAndRegistration` (`text.find(" first\n")` missing)
through a scheduling race on the one-shot drain:

- run 36100836920 (b446c6d, Release): failed 2026-09-25T05:58Z
- run 36101076093 (8f51b08, Debug): failed 2026-09-25T06:02Z
- run 36103143451 (a1e7923, Debug): failed 2026-09-25T06:30Z

Run 4000191 (4000191) passed only because the one-shot drain happened to run
after the test log calls. The earlier failure on run 36097349433 (11e6111) was
an unrelated CMake configure error (missing nlohmann_json), fixed by 4000191.

## Fix

`src/common/async_logger.h`: `start()` now executes
`running_.store(true, std::memory_order_release);` before spawning the worker
thread.

## Verification

In WSL: `cmake --build build -j && ctest --test-dir build` passed 10
consecutive runs of contract_tests.

## Comments

- 2026-09-25: Diagnosed from CI logs; fixed with a one-line change and the
  ticket committed together.