---
status: accepted
---

# Separate venue implementations while preserving worker ownership

Keep one public venue adapter per existing integration, move venue-specific code into subdirectories, and split SimNow internally into market-data and execution modules. Preserve the current SDK callback queues and business worker ownership. This makes state ownership easier to maintain without introducing independently configured clients or routing before the application needs them.

Keep lifecycle orchestration in the application entry point, extracting functions rather than introducing a runtime manager. ADR 0001 describes adapter-owned startup/shutdown as an earlier arrangement; the current implementation already places that orchestration in main. This decision preserves the current arrangement and ADR 0001's separate market worker.

Expose recorded-failure queries on the common adapter interface, with default false/empty results and SimNow overrides. Preserve Binance and Simex fatal-error policies rather than turning an interface cleanup into a recovery redesign. Absence of a recorded failure is not a health guarantee.

The user confirmed these decisions on 2026-10-14. Implementation and offline validation completed in the same session; see [the design](../design/venue-adapter-refactor.md).
