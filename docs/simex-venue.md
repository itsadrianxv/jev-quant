# Simex venue integration

Jev connects to an independently running simex process through localhost TCP orders and UDP market data. The trading process uses the real OpenRouter-backed Jev provider. Binance remains the default venue in config.json.

## Build

Use 64-bit little-endian Linux or WSL with compatible GCC/Clang ABIs. Both projects require their normal CMake dependencies. From the jev-quant repository:

```sh
cmake -S ../simex -B ../simex/build/jev-integration -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build ../simex/build/jev-integration -j 4
cmake -S . -B build/simex-integration -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/simex-integration -j 4
ctest --test-dir ../simex/build/jev-integration --output-on-failure
ctest --test-dir build/simex-integration --output-on-failure
```

`JEV_SIMEX_SOURCE_DIR` defaults to `../simex`; set it explicitly for another checkout. `cmake/SimexProtocolLock.cmake` records normalized SHA-256 fingerprints of the shared v1 wire definitions based on f5b7ddb, with relative includes and explicit ABI assertions. CMake rejects protocol drift. The accompanying runtime/transport fixes are required: an unmodified f5b7ddb service is not the supported server. No matching-engine code is linked into Jev. Use `-DJEV_ENABLE_SIMEX=OFF` to build only the existing Binance integration without a simex checkout.

The existing single-repository CI explicitly disables simex because it does not check out the companion repository. Run the commands above to validate the simex-enabled build.

## Run separately

Keep the real OpenRouter credentials in the existing local `.env`. Simex mode does not load Binance credentials. In one terminal:

```sh
../simex/build/jev-integration/simex_server ../simex/server.json
```

In another terminal:

```sh
./build/simex-integration/jev_trading config.simex.json
```

Both commands respond to SIGINT/SIGTERM. The server supports one connection per process and exits after the participant disconnects. Start a fresh server for another run.

`server.json` selects the instrument profile, reference price, fixed trading day, participant id, TCP port, UDP destination port, snapshot interval, queue capacity, and maximum process lifetime. The profile path is relative to server.json. The server runs REALTIME in the continuous session without scheduled rollover. It starts with no orders or positions and supplies no synthetic liquidity. Market-data and counterparty generation remain the user's separate simex work.

Match `venue.tcp_port`, `venue.udp_port`, and `runtime.client_id` to the server configuration. The adapter maps the configured local ticker to simex ticker 0. `feed_timeout_ms` must exceed the server snapshot interval; snapshots also establish feed liveness when the book is empty. `runtime.run_seconds=0` keeps normal operation running until interrupted. The server's lifetime bounds the session to at most one day; stop before changing the venue trading day.

## Live validation

```sh
python3 scripts/simex_live.py --duration 60
```

This explicitly invoked runner starts both real processes, waits for server readiness, saves process and component logs, and cleans up on completion or failure. It never probes TCP readiness by consuming the server's sole connection. Each run writes a separate directory under `.scratch/simex-venue-adapter/`, including `result.json`. Use `--server`, `--server-config`, `--trading`, `--config`, and `--output` to override paths.

Exit codes describe the evidence:

| Code | Result |
| --- | --- |
| 0 | Real Jev decision, order execution, and participant position update observed |
| 1 | Startup, process, transport, or runner failure |
| 2 | No usable market data; decision chain and trading loop unverified |
| 3 | Usable market data, but no accepted real Jev decision |
| 4 | Decision chain passed; no execution, so the trading loop remains unverified |

Provider failures are counted separately. Hold is a valid model decision, not proof of order execution. The current Jev decision path submits LIMIT orders; adapter support for MARKET does not mean a live run covers MARKET, rejection, cancellation, or partial execution. No independent deterministic simex integration suite is included. Existing component and regression checks still run.

## Participant behavior

- Start the configured participant with zero positions and no active orders. The protocol has no account query, so this is an operating precondition. Do not restart only Jev against retained venue state.
- The adapter rebuilds the venue order-level book and publishes top-five depth snapshots. Initial synchronization installs a complete snapshot and applies contiguous buffered increments beyond its watermark. Later snapshots do not conceal sequence gaps.
- A valid bid and ask with positive quantities and bid below ask are required for evaluation. Empty or one-sided depth suspends new evaluations and orders. Already running HTTP calls may finish, but their invalidated decisions are discarded. Order responses continue to update positions.
- Simex prices and quantities remain integers. LIMIT maps to DAY, MARKET to the venue's immediate MARKET/DAY behavior. OPEN, CLOSE_TODAY, and CLOSE_YESTERDAY map explicitly; generic CLOSE is rejected. Existing close allocation chooses a concrete position day.
- Both partial and complete executions map to FILLED, with execution delta and remaining quantity. Requests and responses have independent transport sequences. One gateway thread owns transport, local rejections, private response publication, and market publication.
- Opposite-side OPEN is blocked while a position or an outstanding opening order could create simultaneous long and short holdings. Pending cancellation still counts as outstanding. Close first; OPEN is never silently converted to CLOSE.
- Missing last trade, session volume, price limits, open interest, and account amounts are sent to Jev as unavailable values, not invented market observations. PnL remains the core's internal price-difference-times-quantity measure, without currency conversion or contract multiplier accounting.
- Disconnects, malformed frames, inconsistent book updates, sequence gaps, bounded-buffer overflow, and feed timeout are fatal. Orders with unknown execution status are not retried. Stopping locally does not cancel working orders at the venue.

## Companion simex corrections

The shared queue publishes slots with atomic acquire/release synchronization. Snapshot reads and mutations use a mutex. Queue overflow fails the runtime instead of dropping the remaining output batch, so the runtime and publisher sequence counts cannot silently diverge. TCP uses bounded nonblocking polling for partial frames, independently sends asynchronous responses, and joins with an idle client still connected. Private responses are restricted to the configured participant. UDP sends are checked, snapshot datagrams are size-bounded, and the standalone server observes runtime/transport health.

The native wire layout is unchanged and remains platform-specific. Header assertions and source fingerprints detect local compatibility drift; they are not a remote negotiation or recovery protocol.
