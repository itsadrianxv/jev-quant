# Per-Producer Asynchronous Logging

Status: accepted

The trading system uses one bounded single-producer/single-consumer queue per registered producer and one background logger thread that polls all queues. Each producer writes to its own file, while producers format fixed-size text records containing the event timestamp, level, component, and message. Queue overflow drops the record for every level, and messages longer than the fixed buffer are truncated, because this first implementation is diagnostic logging rather than an audit log and must not block trading threads. Producers must be registered before the logger starts and must be stopped and joined before the logger is stopped.

## Considered Options

- A shared multi-producer queue was rejected to preserve the existing SPSC queue contract and avoid a shared producer contention point.
- One logger thread and file per producer was rejected because it scales threads and file handles with the number of producers.
- Blocking or synchronous fallback for full queues was rejected for the initial implementation because it would make logging latency unpredictable on trading paths.

## Consequences

The logger preserves ordering within each producer file but provides no global ordering across files. Records can be silently lost under load, so the logger cannot serve as an audit trail.
