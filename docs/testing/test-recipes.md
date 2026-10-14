# Per-feature test recipes

The test pyramid for this repository, feature by feature. Each recipe lists
the scenarios a feature must keep covered and the layer each scenario lives
in. Layers mirror the CTest labels:

- **unit** — in-memory value/state tests, no threads, files, sockets, or
  wall-clock waits (`tests/unit/`, label `unit`).
- **component** — multi-component flows through participant-boundary seams,
  still in-memory (`tests/component/`, label `component`).
- **integration** — real dependencies (files, threads, sockets, the CTP
  transport fake); bounded waits everywhere (`tests/integration/`, label
  `integration`).

New work should add each new scenario to the matching recipe below instead of
accreting monolith tests. Suite names are the unit of work under test; test
names read as scenario-plus-expectation sentences.

## Common primitives

- Lock-free queue (`LFQueueTest`, unit): empty queue has nothing to read,
  FIFO slot round-trip, full queue rejects writes, consumed slots free
  capacity, zero capacity throws.
- Queue concurrency (`LFQueueSpscTest`, integration): single-producer/
  single-consumer stress conveys 10,000 messages in order under a bounded
  deadline.
- Message builder (`MessageBuilderTest`, unit): fresh buffer empty, chained
  append formatting, zero capacity never touches storage, one-byte capacity
  holds only the terminator, empty appends, exact fit with adjacent-byte
  guards, signed/unsigned integer limits, integer truncation, truncation at
  capacity, and appends after truncation dropped.
- Log level names (`LogLevelNameTest`, unit): all four record labels and the
  unknown-level fallback.
- Async logger (`AsyncLoggerTest`, integration): fixed-buffer formatting and
  truncation, per-producer files, overflow drops, late-registered producer
  visibility after start, worker-thread binding. Overflow is forced before
  starting the consumer so scheduling cannot change the expected result.
- Async logger boundaries (`AsyncLoggerContractTest`, integration): invalid
  configuration, zero poll interval, component-name boundaries, file-open
  failure with subsequent registration, FIFO shutdown drain, empty/exact-fit/
  oversized messages through both logging APIs, formatter skipped on overflow,
  destructor drain of all queues, repeated start/stop, stop before first start,
  restart and registration rejected after shutdown, first-write binding,
  foreign-thread writes ignored, and handle move construction/assignment.
  Tests use unique temporary directories with fixture cleanup, join producers
  before shutdown, and compare complete records after closing files. Timestamp
  checks cover syntax only, not elapsed time. Binding timeout outcomes remain
  deferred because the current API returns no completion status.
- Component logging (`ComponentLoggingTest`, integration): externally registered
  handles transferred into worker components, lifecycle file routing, deferred
  market publisher binding, Binance construction without binding, separate
  ordinary/TTT worker channels, explicit disabled handles, and offline Binance
  snapshot/depth publication on the market worker independently of the gateway,
  with fill/rejection publication on the gateway worker. See
  [component logging](../component-logging.md) for ownership and lifetime rules.
- Memory pool (`MemPoolTest`, `MemPoolPlacementTest`, unit): distinct block
  handout, LIFO reuse, value-initialization on revive, zero capacity throws,
  placement-new argument forwarding, reallocation resets.
- Message rendering (`MessageRenderingTest`, unit): `MarketUpdate`,
  `ClientRequest`, `ClientResponse`, and `OMOrder` renders carry the
  identifying type/offset text.

## Market data books

- Participant order book (`MarketOrderBookTest`, unit): fresh BBO invalid,
  snapshot-derived BBO, mode-protection throws both directions, aggregated
  adds, modify within/across levels, cancel draining, re-add replacement,
  unknown-id modify/cancel no-ops, snapshot-boundary and trade no-ops, clear
  and re-use, order-slot exhaustion fail-fast, cleared slots reusable, level
  exhaustion per side with the opposite side unaffected, freed level slots
  reusable, modify-at-exhaustion keeps the resting order, backward-shift
  compaction for colliding orders and levels, and seeded 20,000-operation
  equivalence against an ordered-map reference model.
- Binance depth book (`BinanceDepthBookTest`, unit): fresh book unready,
  pre-snapshot event buffering in the pending ring, buffered events applied
  in order at snapshot install, stale events skipped, in-place level update,
  zero-qty level removal, malformed event throws, snapshot sorting,
  reinstall replaces every level, published update price scaling in level
  order, sequence-gap throw with book unchanged, overlapping update ranges
  tolerated, pending ring bounded at capacity, and published updates equal a
  reference model under a fixed seed.

## Position, risk, and orders

- Position info (`PositionInfoTest`, unit): fresh position, open buy/sell
  accumulation with VWAP, yesterday-first close allocation, generic close
  consuming yesterday then today, close-today isolation, zero/invalid
  quantity ignored, unrealized PnL from BBO mid for longs and shorts, flat
  position has zero PnL.
- Position keeper (`PositionKeeperTest`, unit): non-filled responses
  ignored, filled responses accrue the ticker position.
- Risk manager (`RiskManagerTest`, unit): parameterized pre-trade risk truth
  table; null position keeper yields invalid.
- Order state machine (`OrderStateMachineTest`, component): new order enters
  pending-new, accepted promotes to live, canceled/rejected kill, partial
  fill keeps live with reduced quantity, full fill kills, cancel rejection
  returns pending-cancel to live, no-op moves on matching/live/pending-new/
  pending-cancel, replacement after completed cancel, late cancel rejection
  leaves the newer order untouched, invalid move arguments place nothing,
  close prefers yesterday then today, clamped to closeable quantity, skipped
  without position or when blocked by the loss limit.

## Decision handling (TradeEngine + Jev pipeline)

- Dispatch order (`TradeEngineDispatchTest`, component): responses drain
  before market updates before decisions.
- Decision matrix (`TradeEngineDecisionMatrixTest`, component): unknown/zero/
  stale evaluation ids dropped, out-of-range ticker dropped, no valid
  features dropped, venue-not-ready dropped, working order blocks until the
  venue is flat, opposite-position opens blocked, outstanding-opposite-order
  opens blocked, open-long buys at the ask, open-short sells at the bid,
  configured quantity carries, close-long sells at the bid, close-short buys
  at the ask, close while flat is a no-op, hold places nothing.
- Evaluation scheduling and mapping (`EvaluationSchedulingTest`,
  `FeatureSnapshotTest`, `IncrementalFeatureTest`, `SimexGuardTest`,
  `DecisionExpiryTest`, `WorkerFailureTest`, `EvaluationSnapshotTest`,
  component): evaluation scheduled per snapshot, decision-to-order mapping
  (client id, side, price, offset), valid snapshots and recovery across all
  three venue modes, per-ticker evaluation-state builds, incremental feature
  updates, simex readiness/position gates on evaluations and open decisions,
  stale decision expiry, provider failure counting, evaluation state carries
  position and risk.
- Feature engine (`FeatureEngineTest`,
  `FeatureEngineInvalidTopOfBookTest/TruthTable`, unit): invalid until first
  valid update, mid/spread/quantity/imbalance computation, bid-heavy
  imbalance sign, near-sentinel quantities stay valid, invalid truth table
  (locked, crossed, zero, negative, sentinel prices; zero/sentinel
  quantities), stays reset until a valid update.
- Decision queue (`JevDecisionQueueTest`, unit): empty queue has nothing to
  read, published decision readable exactly once.
- Decision HTTP request body (`JevHttpRequestBodyTest`, unit): flat position
  selects open/hold criteria, occupied selects increase/close criteria,
  null volume and limit prices, live working orders ship attributes, dead
  working orders omitted, top-level `evaluation_id` present and `endpoint`
  absent, evaluation state ships scaled features with `position`/`risk`/
  `account` keys and depth reduced to `last_price`.
- Decision HTTP response parser (`JevDecisionParserTest`, unit): flat
  response parses to hold, occupied response parses to close, and the
  failure branches (missing answers object, malformed payload, missing bias
  answer, non-choice answer, answer without choice, unknown bias/intent
  choice, missing probabilities/confidence, out-of-range confidence and
  probabilities, occupied position without close probability) each assert
  the thrown error type.
- Previous-wording variant (`JevWordingVariantTest`, `JevWordingDiffTest`,
  unit): flat/occupied variants rewrite only the instruction wording, diffs
  carry the expected pointers, diff tool reports leaf differences as
  from/to and key-set/array-length differences as errors.
- Binance UM futures adapter mapping (`BinanceUmFutures*Test`, unit):
  parameterized order-mapping truth table, parameterized order-status truth
  table, adjusted-timestamp truth table, and the rejection branches
  (wrong ticker, cancel request, unsupported order type, invalid quantity or
  side).

## Decision transport and capture (integration layer)

- Decision HTTP client end to end (`JevHttpDecisionPathTest`, integration):
  full evaluate path against the local mock decision server spawned with a
  bounded readiness probe.
- Diagnostic capture (`JevHttpCaptureTest`, integration): disabled by
  default, attempt files and metadata recorded for enabled captures
  (including sha256 test vectors), failed attempts recorded without leaking
  `Authorization` material.

## SimNow venue adapter (integration layer)

- Credentials (`SimNowCredentials`): missing credentials without private
  values, quoted/commented dotenv parsing, environment precedence, empty
  environment value treated as unset, unterminated quotes rejected without
  value leaks, empty dotenv values rejected.
- Adapter lifecycle and order flow (`SimNowAdapter`): subscription on
  startup, foreign-session market login timeout, dirty position and working
  orders refuse startup, missing instrument refuses startup, throttle
  rejection retries the startup query, queue overflow before start throws,
  first order uses a sequential ref with open offset, duplicate order-status
  events publish one acceptance, new order while another outstanding is
  rejected, open until execution status known is rejected, open while
  position held is rejected, close uses close-today, shutdown cancels the
  live order and reports canceled, cancel-filled at shutdown reports the
  execution, unconfirmable shutdown cancel fails, disconnect fails and
  rejects further requests, trading-day change fails, insert SDK failure and
  rejected submit status report rejections, off-tick price rejected.
- CTP price conversion (`CtpPriceConversion/MapsUnconvertibleInputsToInvalid`,
  parameterized): unconvertible price/qty inputs map to invalid sentinels.
- Venue engine contract (`SimNowVenueEngine`): open decision forwarded to the
  request queue, venue order held instead of repriced, new opens blocked
  while position held, unrealized PnL reported with depth omitted from the
  evaluation state, account and multiplier refreshed in later evaluations,
  decisions dropped while venue trading is disabled.

## Tick-to-trade instrumentation (integration layer)

- `TttInstrumentationTest`: stamped messages produce the four latency events
  with `elapsed_ns=` values; unstamped messages produce none.

## Market and order worker separation (integration layer)

- `MarketDataWorkerTest`: duplicate start, serialized lifecycle, callback
  configuration, joined stop/destruction, and rejected restart.
- `ComponentLoggingTest`: market worker log ownership and offline Binance
  snapshot/depth progress before the order worker starts.
- `SimNowAdapter`: depth processing during blocked order submission, quote
  invalidation and recovery, market-side disconnects, startup trading-day
  mismatch, plus existing cancellation and failure tests.
- `SimexAdapter` (requires `JEV_ENABLE_SIMEX=ON`): loopback UDP snapshots with
  the gateway stopped; TCP new orders despite unusable depth, with invalid
  quantity and duplicate-order rejection retained. No live venue is used.
