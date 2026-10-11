# Deterministic market features

TradeEngine owns one FeatureEngine per instrument and updates it after each market update has changed the local order book. The engine thread copies the resulting values into each scheduled JevEvaluationState. JevWorker reads this copy, so later market updates cannot change an in-flight request.

For best bid B, best ask A, and their quantities Qb and Qa:

| Request field | Definition | Unit |
| --- | --- | --- |
| features.mid_price | (B + A) / 2 | Same price unit as other Jev request prices |
| features.relative_spread_bps | (A - B) / mid-price * 10000 | Basis points |
| features.top_of_book_total_qty | Qb + Qa | Instrument quantity |
| features.top_of_book_qty_imbalance | (Qb - Qa) / (Qb + Qa) | Dimensionless; positive means more bid quantity |

The four values form one valid or invalid set. Both prices and quantities must be present and positive, and B must be less than A. Invalid input clears the feature state, suspends scheduled evaluations, and invalidates the pending evaluation id. The worker skips invalidated waiting states, and the engine rejects invalidated decisions even after the book recovers. Private order responses continue to update orders and positions. Valid market updates allow scheduled evaluations to resume.

The Jev request replaces depth.bids and depth.asks with these features. Other depth metadata, positions, working orders, account state, and risk limits remain available. The local order book remains available for order execution. Binance, SimNow, and Simex use the same feature computation and validity rules.

Feature validity does not assert freshness. This version has no feature timeout, historical windows, microprice, or aggressor-trade ratio. It adds no feature thread or queue. Existing venue readiness and evaluation scheduling still apply. Feature correctness and integration are tested deterministically; the change makes no claim about improved trading performance.
