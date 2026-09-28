# Running the SimNow adapter

Build on Linux x64 with `JEV_ENABLE_SIMNOW=ON`, the default. Set it to `OFF`
when the vendored CTP runtime is not wanted. Both CI presets compile and test
the adapter without making network connections.

Copy `config.simnow.json` to a local configuration and replace the instrument
placeholder with a currently tradable SHFE futures contract. The default
fronts are the documented SimNow 7x24 pair; front availability and supported
contracts must be checked against SimNow when running. Put `SIMNOW_USER_ID`
and `SIMNOW_PASSWORD` in `.env` in the process working directory, or export
them in the environment. Nonempty environment values take precedence per key.
The dotenv reader supports UTF-8 BOM, CRLF, optional `export`, quoted values,
and comments; it does not expand shell variables or execute shell code.
Missing or empty credentials fail before connecting, without printing values.
Credentials must not be committed. The official SimNow broker, AppID, and test AuthCode are used.
Keep the configured flow directory private to this account and process.

The first version requires an empty, exclusively controlled account and
one-contract risk limits. It confirms settlement automatically. Startup
queries reject any existing position or active order across the account.
Only ordinary, currently tradable SHFE futures are accepted.

To verify login and startup without submitting orders, run:

```sh
./build/Debug/simnow_live_smoke --check-startup config.simnow.local.json
```

This still confirms settlement and queries the account, and requires usable
market data. It can time out outside trading hours and does not prove execution.

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

The dotenv follow-up also passed both cold builds and both ctest targets per
configuration. Configuration tests cover environment precedence, file fallback,
quoted values, CRLF/BOM, absent credentials, and errors that omit secret values.

On 2026-09-28, `--check-startup` passed against the trading-session fronts
`182.254.243.31:30001/30011` with `rb2610` after the account password reset.
Authentication, trader/market login, settlement confirmation, instrument and
funds queries, account-wide empty-position/active-order checks, and market-data
subscription reached readiness. The result was:

```text
event=simnow_stopped position=0 unresolved=0
event=simnow_startup_result ready=1 orders_submitted=0 position=0 failed=0
```

This run exposed an MD login callback with `request_id=0`, `last=1`, and
`error=0`, despite a nonzero submitted ID. The adapter now matches that one
case to its sole pending MD login. A regression test reproduced the startup
timeout before the fix and passed afterward; completed-login duplicates and
unrelated nonzero IDs do not initiate another subscription.

The check ran during the midday break and submitted no orders. A usable
snapshot at startup does not prove an open trading session.

The deterministic order check later ran during the night session and passed:

```text
event=simnow_response order=1 type=FILLED exec_qty=1
event=simnow_response order=2 type=FILLED exec_qty=1
event=simnow_response order=3 type=ACCEPTED exec_qty=0
event=simnow_response order=3 type=CANCELED exec_qty=0
event=simnow_smoke_result round_trip=1 canceled=1 position=0 failed=0
```

The Jev runtime also connected and produced a live decision. Its periodic funds
refresh then timed out, so the adapter disabled trading and the process stopped
with `position=0 unresolved=0`. A following no-order startup check passed again.
The real Jev run therefore does not count as a completed Jev trading round trip;
the deterministic adapter round trip is verified, while the funds-query timeout
remains a live-session issue to investigate.
