# Component logging

`trading_main.cpp`, benchmark setup, and test fixtures own `AsyncLogger`,
register producers, and transfer the move-only handles into components.
Component constructors accept optional handles by value and move them into
local storage. Omitting a handle disables that channel. Components cannot
configure, register with, start, or stop a logger.

| Component | Producer label | File | Owning thread |
| --- | --- | --- | --- |
| MarketDataConsumer | MarketData | `market-data-<client_id>.log` | Market update publisher |
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

`MarketDataConsumer::start()` only sets the running flag. The first publication
or sequence check writes `component_started` on the publisher thread, before
any update or gap record. This avoids binding a handle on a composition thread.
A consumer that never receives a publication or sequence check emits no startup
record. `OrderGateway::publishClientResponse()` and `processPending()` must run
on the same owning thread when sharing its handle.

The Binance adapter gives each WebSocket worker a bounded 1,024-slot SPSC event
queue. The gateway worker drains those queues, publishes the initial depth
snapshot and subsequent updates, handles user events, and publishes REST
responses. Response sequence numbers come from the next expected response,
independently of request sequence numbers. The queues terminate on overflow,
matching the existing fail-fast business-queue policy. JSON event copies can
allocate; decoding into typed events remains a TODO. Venue lifecycle logs stay
on the thread that calls `start()` and `stop()`; its constructor does not bind.
Its destructor stops and joins producers before destroying callback state.

The live composition registers the Venue channel only for Binance. Simex and
SimNow keep publication on their gateway workers. The synchronous benchmark
registers only TTT, preserving its existing channel selection. Tests register
only the channels they exercise. No component waits for producer counts;
TTT and market handles may remain unbound until the first message.

Keep the logger alive for the entire component lifetime. Stop and join every
producer before stopping the logger so its final drain includes pending log
records. Logger handles expose only producer operations and do not own the
logger or its queues. `ProducerHandle::MessageBuilder` and existing event text
remain unchanged.

Offline coverage is in `ComponentLoggingTest`, `AsyncLoggerTest`, and
`TttInstrumentationTest`. Worker tests use joins or bounded futures before
stopping the logger and inspecting files. The Binance queue test uses an
offline friend harness to inject the same events the WebSocket callbacks enqueue.
