# Component logging

`trading_main.cpp`, benchmark setup, and test fixtures own `AsyncLogger`,
register producers, and transfer the move-only handles into components.
Component constructors accept optional handles by value and move them into
local storage. Omitting a handle disables that channel. Components cannot
configure, register with, start, or stop a logger.

| Component | Producer label | File | Owning thread |
| --- | --- | --- | --- |
| MarketDataConsumer | MarketData | `market-data-<client_id>.log` | Market worker or synchronous publisher |
| OrderGateway | OrderGateway | `order-gateway-<client_id>.log` | Gateway worker or synchronous caller |
| TradeEngine | TradeEngine | `trade-engine-<client_id>.log` | Engine worker or synchronous caller |
| TradeEngine TTT | TTT | `ttt-<client_id>.log` | Same engine caller, separate queue |
| JevWorker | JevWorker | `jev-worker.log` | Jev worker or synchronous caller |
| Binance adapter | Venue | `venue-<ticker_id>.log` | Caller of start and stop |

Registration leaves a handle unbound. Worker components bind ordinary logs in
`run()`. First use binds synchronous and TTT logging, including
`TradeEngine::processPending()` without starting a worker. Moving a bound
handle does not change its owner. A component with active logging must not
switch between synchronous execution and a different worker thread.

`MarketDataConsumer::start()` creates its worker. The worker binds its handle
and writes lifecycle records even if it receives no market events. Synchronous
publication without starting a worker still binds on first use. Lifecycle calls
must be serialized by the owner. A running start is a no-op, stop joins and is
repeatable, and restart after a completed stop throws `std::logic_error`.
`running()` reports run state, not market readiness. The adapter installs the
market poll callback before start and clears it after stop. Market polling uses
bounded batches; stop does not promise to drain pending market input.
`OrderGateway::publishClientResponse()` and `processPending()` must run on the
same owning thread when sharing its handle.

The Binance adapter gives each WebSocket worker a bounded 1,024-slot SPSC event
queue. The market worker publishes the initial depth snapshot and drains depth
events. The gateway worker drains user events and publishes REST responses.
Response sequence numbers come from the next expected response,
independently of request sequence numbers. The queues terminate on overflow,
matching the existing fail-fast business-queue policy. JSON event copies can
allocate; decoding into typed events remains a TODO. Venue lifecycle logs stay
on the thread that calls `start()` and `stop()`; its constructor does not bind.
Its destructor stops and joins producers before destroying callback state.

The live composition registers the Venue channel only for Binance. Simex and
SimNow also publish market updates on their market workers and order responses
on their gateway workers. Simex keeps UDP/book state separate from TCP/order
state and no longer rejects new orders based on market usability. SimNow keeps
readiness, trading-day checks, and failure coordination inside the adapter;
neither worker component references the other. Venue adapters still orchestrate
worker lifetimes; moving orchestration to main is a follow-up TODO.
The synchronous benchmark registers only TTT, preserving its existing channel
selection. Tests register
only the channels they exercise. No component waits for producer counts;
TTT and synchronous market handles may remain unbound until the first message.

Keep the logger alive for the entire component lifetime. Stop and join every
producer before stopping the logger so its final drain includes pending log
records. Logger handles expose only producer operations and do not own the
logger or its queues.

The logger has a single start/stop lifecycle. Calling `start()` while running
or calling `stop()` repeatedly is a no-op. Calling `stop()` before the first
start leaves queued records and producer registration available. Once a
started logger has stopped, `start()` and `registerProducer()` throw
`std::logic_error`; create another logger for another session. Destruction
stops a started logger and drains its queues. A logger that was never started
does not drain queued records.

Messages retain at most 1,023 bytes plus a NUL terminator. The standalone
`ProducerHandle::MessageBuilder` also accepts zero capacity, in which case it
never accesses storage. Queue overflow drops the new record; `logFixed()`
does not call its formatter when no slot is available.

Offline coverage is in `ComponentLoggingTest`, `AsyncLoggerTest`,
`AsyncLoggerContractTest`, and
`TttInstrumentationTest`. Worker tests use joins or bounded futures before
stopping the logger and inspecting files. The Binance queue test uses an
offline friend harness to inject the same events the WebSocket callbacks enqueue.
