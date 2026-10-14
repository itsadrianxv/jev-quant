# SimNow performance experiments

## Timestamp reference

| 时间戳 | 定义 |
| --- | --- |
| t1 | 进入 CTP 行情回调，不是网卡收包。 |
| t2 | MarketDataConsumer 所在线程取得适配器队列中的行情事件。 |
| t3 | 标准化行情即将发布到交易引擎队列。 |
| t4 | 交易引擎取得该条标准化行情。 |
| t5 | 实际订单请求即将发布到 OrderGateway 队列。 |
| t6 | OrderGateway 取得订单请求。 |
| t7 | OrderGateway 即将调用适配器提交接口。 |
| t8 | 适配器即将调用 CTP 下单接口，不是网卡发包；撤单则使用撤单接口前的时间。 |

See performance-benchmark.md for source association, interval meaning, successful-submission selection and Callgrind limitations.

## Prepare before the trading session

Use Linux x64/WSL, the bundled CTP SDK, libcurl/OpenSSL and the normal build dependencies. Install Valgrind including its headers before configuring. A build without those headers supports native capture and rejects Callgrind mode. Use an exclusive empty SimNow account and an eligible current SHFE contract; keep the existing one-contract risk constraints. Use the existing real Jev provider configuration and credentials. No scripted provider or forced trade trigger is added.

Build with symbols for attribution:

```sh
cmake --preset local-gcc-release-profile
cmake --build --preset local-gcc-release-profile --parallel 3
```

Use a local SimNow JSON config. Keep credentials in the existing environment/.env mechanism. Set runtime.run_seconds to zero for manual termination, or at least window + grace. For the example below, use 430 seconds or longer. Set evaluation_interval_ms deliberately: evaluations can become stale while the provider is busy. Record this setting rather than changing freshness rules to obtain more orders.

The runner injects this configuration into a temporary private file, preserving the real provider and venue settings:

```json
{
  "performance": {
    "enabled": true,
    "mode": "native",
    "window_seconds": 300,
    "grace_seconds": 120,
    "capacity": 10000,
    "output_directory": "performance-run"
  }
}
```

The window starts after venue readiness. Choose grace to cover the configured provider timeout/retry envelope plus queue/handling time. A finite grace can omit slow chains. Increase it in a later, separately recorded run if necessary. Capture completion does not stop trading; stop the application normally after the grace period to export data. Early shutdown is an incomplete capture.

Before live use, run the local tests and the Callgrind collection check:

```sh
ctest --preset test-ci-gcc-release
python3 scripts/profiling/check_callgrind_scope.py --binary build/Release-profile/callgrind_scope_test
```

These tests verify instrumentation contracts with controlled test inputs. They are not an alternative offline benchmark or evidence of SimNow performance.

## Native run during market hours

From the directory containing the usual .env, with a fresh output directory and the actual source revision:

```sh
python3 scripts/profiling/run_simnow.py \
  --binary build/Release-profile/jev_trading \
  --config config.simnow.local.json --revision SOURCE_COMMIT \
  --mode native --window 300 --grace 120 --capacity 10000 \
  --out results/simnow-native-001
python3 scripts/profiling/summarize_simnow.py results/simnow-native-001 \
  > results/simnow-native-001/summary.json
```

The runner does not invent a provider result and can submit simulated orders under the normal strategy. It records executable/config hashes, revision, UTC bounds, platform, affinity and selected runtime settings in run.json. It redirects normal application logs into the run directory and deletes the temporary config afterward. It does not archive .env or the full config. Record the compiler/build type, host load, VM resources, trading session, contract, provider/model and logging policy alongside the run. Never include credentials in this note.

Inspect capture.json and console.log before interpreting samples.csv. Confirm healthy venue shutdown, zero observation overflow, completed window and nonzero NEW samples. Inspect provider failures and business diagnostics. A successful script exit alone is not acceptance. Review OPEN and CLOSE_TODAY coverage separately; absent categories remain unverified. No complete submission means repeat during another session without forcing a mock decision.

The summary reports adjacent differences and t8-t1 for NEW, t5-t8 intervals for CANCEL. It rejects malformed, duplicate, incomplete-window, overflowed or unhealthy captures. Small-sample percentiles are descriptive only. Compare distributions across several sessions with their conditions recorded; different market/provider inputs prevent a controlled speedup claim. Do not label observed order rate as maximum throughput.

## Callgrind run during market hours

Use a fresh process and directory. Keep the real SimNow/provider path. First establish that the SDK can login and remain healthy under Valgrind; if not, retain the failure and mark this acceptance item pending. Do not replace the SDK with a fake to satisfy it.

```sh
python3 scripts/profiling/run_simnow.py \
  --binary build/Release-profile/jev_trading \
  --config config.simnow.local.json --revision SOURCE_COMMIT \
  --mode callgrind --window 300 --grace 120 --capacity 10000 \
  --out results/simnow-callgrind-001
```

The runner supplies --tool=callgrind --collect-atstart=no --separate-threads=yes --cache-sim=no --branch-sim=no. Do not add --toggle-collect or --instr-atstart=no: those would conflict with the application controls. The collection scopes run only on the engine thread. Inspect each callgrind.out.* file with callgrind_annotate and identify the nonzero engine profile by its book/decision call paths:

```sh
callgrind_annotate --inclusive=yes --tree=both ENGINE_PROFILE_FILE > engine-inclusive.txt
callgrind_annotate --inclusive=no ENGINE_PROFILE_FILE > engine-self.txt
```

Keep all raw thread files. Review profile_market_events and profile_decision_events in capture.json; they count scope entries, not successful orders. Check that polling, HTTP and gateway work have not contaminated the selected profile. Do not use summarize_simnow.py on Callgrind captures; it deliberately rejects their elapsed times. A healthy run with no complete NEW chain is useful collection evidence but does not finish the agreed live acceptance.

## Acceptance record

Record native and Callgrind runs independently: revision/hash, conditions, process return code, startup/login outcome, provider failures, connection/queue errors, window completion, observation overflow, complete NEW count, OPEN/CLOSE_TODAY coverage, and the selected engine profile. Retain raw artifacts before writing conclusions.

Current live status: pending market-hours execution. Local tests do not replace this step. Optimization experiments and before/after claims are outside this delivery.
