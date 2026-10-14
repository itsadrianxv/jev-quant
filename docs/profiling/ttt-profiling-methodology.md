# Retired TTT instrumentation

The legacy recordLatency logs and parser have been removed. Their event names did not consistently describe their boundaries: ttt_order_round_trip measured response-queue waiting, not order RTT.

Use [the SimNow benchmark definition](performance-benchmark.md) and [experiment guide](performance-experiments.md). Historical baseline/ and optimization-after/ files are retained as historical evidence only; they are not current live measurements.
