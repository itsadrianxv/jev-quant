# SimNow venue adapter design

Status: accepted design, 2026-09-28. The operating constraints documented in
`adapter-operations.md` and the implementation and validation approach below
were confirmed by the user. The first version
is implemented in `SimNowVenueAdapter`; live acceptance still requires a
configured SimNow account and successful live test results.

## Implementation

`SimNowVenueAdapter` implements the existing `VenueAdapter` interface.
Keep the CTP market-data and trading API instances, SPI handlers, session state,
order identifiers, and query scheduling private to this module. Build against
the vendored CTP 6.7.11 Linux x64 libraries through an optional CMake target.
Use configuration for the fronts and instrument, and local environment-backed
credentials. Do not select CTP assessment mode merely because SimNow is a
simulation environment. Give each API instance its own flow directory.

CTP callbacks copy their data into separate bounded SPSC queues, one for each
SDK producer thread, and return. The MarketDataConsumer worker consumes market
events, logs in and subscribes through the market API, normalizes depth, and
publishes market updates. The OrderGateway worker consumes trader events and
owns orders, account queries, and response sequencing. Each worker has its own
login request state. Neither SDK callback writes directly to the engine queues.

The adapter protects subscription/quote readiness and trading-day information
with a small mutex. Failure and shutdown flags are atomic; failure text uses the
lifecycle mutex. Only the gateway worker, or its owner after join, publishes
account/readiness state. A market failure disables new submissions immediately
through the fatal flag; the gateway publishes the disabled state on its next
poll. No worker component calls or references the other. Account updates reach
the engine through its existing thread-safe handoff. Queue exhaustion disables
trading and reports failure; private execution events are not silently dropped.

Shutdown first stops and joins the market worker, then allows the gateway to
complete its existing cancellation procedure. Both workers are joined before
the SDK transport is released. Market input is not drained during shutdown.

Serialize startup queries, with at most one outstanding query and a
conservative one-second minimum interval. Complete authentication, login,
settlement confirmation, instrument and account checks, and market-data
readiness before enabling decisions. Refresh funds every 30 seconds after
startup. An unsuccessful refresh marks the account snapshot stale rather
than presenting old values as current.

Normalize market data into existing depth snapshots using only levels that
the feed actually supplies. Validate invalid-price sentinels and quantities;
do not invent missing depth. Validate order prices against the instrument's
tick size. Keep internal prices consistent and expose meaningful monetary
units and contract-multiplier-aware, pre-fee PnL to Jev.

Keep CTP request return codes separate from venue acceptance. Use order
callbacks to track acceptance, rejection, and cancellation, and trade
callbacks to book executions. Deduplicate callbacks and retain order state
until known executions have been delivered, even if a terminal order status
arrives first. Keep CTP string identifiers private and map them to local
numeric order identifiers. An unresolved request never triggers an automatic
resubmission.

The engine enforces one working order globally for the configured instrument,
one contract of exposure, and no opposing open. Working orders retain their
original limit price despite subsequent decisions. Add only the engine
changes required for these rules, funds updates, readiness, and correct PnL.
Leave automatic disconnect recovery and cross-day continuation as explicit
implementation TODOs.

## Validation

Run offline tests for startup gates, callback duplication and ordering,
rejections, cancellation races, disconnect gating, price conversion, and
contract-multiplier PnL. Tests use an internal SDK seam so these cases do not
depend on the availability or matching behavior of SimNow.

Run the required WSL Debug and Release cold builds and ctest suites. Live
acceptance uses official SimNow with the vendored official CTP libraries;
openctp TTS is not a substitute for that acceptance.

First validate market data and a deterministic one-contract open/close and
cancellation sequence, starting from an empty account. Then run the same
adapter through the existing Jev provider, recording its decisions, orders,
and venue callbacks. A Jev run that only holds validates connectivity and
decision delivery but does not establish a completed live trading round trip.
Credentials and explicit authorization to place the live test orders are
required before those tests execute.

## Deferred work

Add implementation TODO comments for automatic disconnect recovery and
reconciliation, and for trading-day rollover and continued operation.
The first version must still stop automatic order submission on disconnect
or trading-day change. Neither condition establishes that an outstanding
order was rejected, canceled, or filled.

Existing positions and orders are not imported on startup. Multiple
instruments, simultaneous long and short holdings, order repricing,
automatic liquidation on shutdown, and local margin and commission
calculation are outside this version.
