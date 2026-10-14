#!/usr/bin/env bash
# Run the current offline protocol without overwriting historical artifacts.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${REV:?Set REV to the source revision captured before synchronization}"
python3 scripts/profiling/run_benchmarks.py \
  --bench "${BENCH:-build/Release-profile/jev_bench}" \
  --revision "$REV" --out "${OUT:-tmp/benchmark-experiment}" "$@"
