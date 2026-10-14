# Venue adapter refactor

Status: implemented and verified on 2026-10-14; awaiting PR review.

## Objective

Make the existing adapters easier to maintain without changing trading behavior. Separate venue-specific dependencies, give SimNow market and execution state clear ownership, and simplify lifecycle code in the application entry point.

The Nautilus research informed the separation of data and execution responsibilities. This change does not introduce independently configured data/execution clients, factories, routing tables, or a multi-venue runtime.

## Agreed scope

- Organize all existing venue-specific code under `binance/`, `simex/`, and `simnow/`.
- Keep common interfaces in `src/trading/venue/`.
- Split SimNow internally into market-data and execution modules behind one public adapter.
- Keep lifecycle orchestration in the application entry point. Extract functions as useful; do not introduce a runtime manager.
- Expose existing recorded-failure queries through `VenueAdapter` without introducing a lifecycle state machine or changing fatal-error policies.
- Preserve all worker ownership, SPSC producer/consumer relationships, and performance trace boundaries.

Keep the current SimNow restrictions: SHFE only, one configured instrument, one-contract orders, one outstanding order, empty-account startup, supported open/close-today behavior, and failure on disconnect or trading-day rollover. Do not add recovery, reconciliation, additional order semantics, or multi-account support.

## Directory structure

```text
src/trading/venue/
  venue_adapter.h
  binance/
    binance_um_futures_adapter.h/.cpp
    binance_depth_book.h/.cpp
    binance_websocket_stream.h/.cpp
  simex/
    simex_venue_adapter.h/.cpp
  simnow/
    simnow_venue_adapter.h/.cpp
    simnow_config.h/.cpp
    simnow_market_data.h/.cpp
    simnow_execution.h/.cpp
    ctp_transport.h/.cpp
    ctp_event.h
    ctp_mapping.h/.cpp
    simnow_control.h/.cpp
```

The SimNow internal file list is a working layout; combine small files where that improves readability. Keep CTP integration here until another concrete CTP integration needs shared code. Do not rename public adapter classes merely to match filenames.

The current `venue_adapter.cpp` contains Binance implementation and moves into the Binance directory. The common header must no longer import Binance configuration, WebSocket, or depth-book implementation headers. The public SimNow adapter header should not expose SDK payload definitions; offline tests explicitly include the internal transport/event headers when needed.

## SimNow ownership

The market-data module owns the market event queue, market login/subscription progress, market request IDs, quote conversion, and market polling. The MarketDataConsumer worker remains its consumer and the publisher of normalized market updates.

The execution module owns the trader event queue, authentication/login/settlement progress, query scheduling and throttling, account/instrument query results, trading session identifiers, order references, orders, fill deduplication, position tracking, submission/cancellation, response publication, and shutdown cancellation progress. The OrderGateway worker remains its consumer.

Move existing order algorithms intact. Preserve acceptance evidence, duplicate handling, terminal-status/fill ordering, immediate SDK rejection, and cancellation completion behavior. Do not replace the current order flags with a new state machine in this change.

The outer adapter owns composition, callback registration, transport lifetime, readiness waiting, and shutdown coordination. Keep the injectable `Ctp::Transport` seam for offline tests. Internal market/execution modules do not need abstract base classes.

Keep shared control state small and synchronized: trading-day agreement, subscription/quote usability, failure flags and first error, readiness notification, and shutdown completion. Retain the existing mutex/atomic/condition-variable relationships. Do not share mutable order containers with the market worker. Preserve the existing lifecycle-owner publication of disabled account state after the gateway has joined.

## Lifecycle and failure interface

The application already owns worker startup and shutdown and reuses a shutdown lambda on normal and exceptional exit. Preserve that ownership and reuse when extracting functions. Preserve partial-start cleanup, readiness ordering, and shutdown ordering, including keeping the gateway alive while SimNow resolves cancellation and the engine alive while responses arrive. Preserve measurement finalization and output ordering.

Add default queries to the common interface:

```cpp
virtual bool failed() const noexcept { return false; }
virtual std::string failure() const { return {}; }
```

SimNow overrides these methods using its existing recorded error. Replace concrete SimNow downcasts used only for failure queries with common-interface calls. Existing `running()` behavior remains unchanged; do not reinterpret it as proof that trading is enabled.

`failed() == false` means no queryable failure has been recorded. It is not a health guarantee. Binance and Simex retain existing exceptions and `std::terminate()` paths. This interface does not make those failures recoverable or queryable after termination. SimNow retains its first error through cleanup. No lifecycle enum or new recovery framework is introduced.

## Implementation sequence

1. Separate the common header and move all venue-specific files. Update source lists and every include/call site; preserve build feature guards.
2. Extract SimNow configuration and SDK event/transport declarations from the public adapter header.
3. Move market and execution state/operations into their respective internal modules, keeping existing behavior and synchronization.
4. Add common recorded-failure queries and simplify application lifecycle functions without adding a manager.
5. Review tests and documentation, then perform the required cold builds. Update the Chinese interview preparation document only for implemented changes.

Complete the related source changes before cold builds. Check the final tree and Git status after moves; do not rely on copy/move success messages. README changes are outside this task.

## Verification

Use existing adapter tests through the public adapter and fake transport interfaces. Add tests only for uncovered observable contracts, not for internal class/file structure.

Preserve coverage for separate SDK producers, market-worker publication, progress while order submission blocks, partial-start cleanup, readiness and trading-day checks, queue overflow, duplicate acceptance/fills, response ordering, immediate submission failures, shutdown cancellation, and failure after disconnect. Verify that common-interface failure queries preserve SimNow failure reporting after cleanup.

Preserve the eight performance observation boundaries and source attribution described in ADR 0002. This refactor makes no latency or CPU improvement claim.

Run WSL Debug and Release cold builds and CTest with parallelism limited to three, using verified Windows-to-WSL synchronization and removing build copies afterward. Ensure the SimNow test target is actually included with SDK support enabled; a default build that excludes SimNow does not validate the split. Preserve Simex conditional-build coverage and the ability to build without optional venues. Live trading is not required for this behavior-preserving refactor.

## Completion criteria

- Common adapter consumers no longer inherit Binance or CTP SDK declarations unintentionally.
- All venue-specific source files live in their corresponding subdirectories.
- SimNow market and execution state have explicit worker ownership; queues and threads are unchanged.
- Existing trading restrictions, order semantics, failure policies, and shutdown behavior remain intact.
- The application queries recorded failure through the common adapter interface.
- Required tests pass, with the tested feature configuration recorded in the PR.

The user authorized implementation of the consolidated design on 2026-10-14. The branch must not be merged without separate explicit approval.

## Validation results

- WSL GCC Debug cold build, SimNow and Simex enabled: 302/302 CTest cases passed.
- WSL GCC Release cold build, SimNow and Simex enabled: 302/302 CTest cases passed.
- WSL GCC Release cold build, SimNow and Simex disabled: 254/254 CTest cases passed.
- Common and SimNow public headers compiled without CTP SDK include paths.
- Source synchronization used local staging, Copy-Item to WSL, and MD5 verification. Builds used at most three compiler jobs. Automatic approval review blocked final recursive cleanup, so the task build directory remains.
- The disconnect test now checks recorded failure through VenueAdapter before and after cleanup. Sixteen migrated method bodies were compared with the original and differ only in shared-control qualification.
- No live venue connection or latency benchmark was run.
