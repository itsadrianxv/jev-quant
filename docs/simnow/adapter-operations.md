# Running the SimNow adapter

Build on Linux x64 with `JEV_ENABLE_SIMNOW=ON`, the default. Set it to `OFF`
when the vendored CTP runtime is not wanted. Both CI presets compile and test
the adapter without making network connections.

Copy `config.simnow.json` to a local configuration and replace the instrument
placeholder with a currently tradable SHFE futures contract. The default
fronts are the documented SimNow 7x24 pair; front availability and supported
contracts must be checked against SimNow when running. Export `SIMNOW_USER_ID`
and `SIMNOW_PASSWORD` in the process environment. Credentials must not be
committed. The official SimNow broker, AppID, and test AuthCode are used.
Keep the configured flow directory private to this account and process.

The first version requires an empty, exclusively controlled account and
one-contract risk limits. It confirms settlement automatically. Startup
queries reject any existing position or active order across the account.
Only ordinary, currently tradable SHFE futures are accepted.

For the deterministic live check, explicitly authorize placing simulation
orders, then run:

```sh
./build/Debug/simnow_live_smoke --place-orders config.simnow.local.json
```

This opens one long contract, closes it, then attempts to cancel a one-contract
buy at the lower limit. The cancellation probe can fill; if so the check fails
and reports the remaining position rather than silently liquidating it.
The check is bounded to 120 seconds after startup. Pending orders are canceled
on exit where the trading connection remains usable. A timeout or disconnect
does not prove that orders were canceled. Inspect the account before retrying.

After the deterministic check passes, configure the existing Jev `.env`
credentials and run:

```sh
./build/Debug/jev_trading config.simnow.local.json
```

Orders retain their original opposite-best limit price until executed,
rejected, expired by the venue, or canceled during shutdown. There is no
automatic repricing, time-based cancellation, or automatic liquidation.
CTP funds refresh every 30 seconds; failed snapshots become unavailable and
disable new trading until a successful refresh. Displayed PnL is CNY before
fees, measured from the process's entry prices, not the broker settlement PnL.

Disconnect or a changed trading day disables trading. Automatic recovery and
cross-day continuation are intentionally deferred. Normal shutdown waits up
to the configured cancellation timeout and reports remaining positions and
unresolved orders. A Jev run containing only HOLD decisions is not proof of
a completed live round trip.

## Validation record

On 2026-09-28, WSL Ubuntu Debug and Release cold builds both passed with
`JEV_ENABLE_SIMNOW=ON` and `JEV_ENABLE_SIMEX=OFF`. Both configurations passed
`contract_tests` and `simnow_tests`, 2/2 ctest targets each. Build parallelism
was limited to two jobs for the 2 GB WSL environment. The final builds emitted
no new compiler warnings. The temporary build directory was removed afterward.

Live SimNow tests were not run because account credentials were unavailable.
The live smoke executable was compiled, but its presence is not evidence of
a successful venue login, cancellation, execution, or Jev trading round trip.
