# SimNow cycle test

This opt-in test asks the real Jev provider to prioritize exercising the trading
chain over profit. It targets 20 one-contract open-fill-close-fill cycles and at
least 40 fills in ten minutes, ending flat. Jev still selects every action,
including HOLD; the engine never substitutes a scripted decision. Existing
readiness, working-order, position and risk checks remain in force.

Run from the application directory in Linux, with its existing `.env` credentials:

```sh
python3 scripts/simnow_cycle_test.py \
  --binary build/Release/jev_trading \
  --config config.simnow.local.json \
  --out results/simnow-cycle-test-YYYYMMDD-HHMMSS
```

The runner requires a new output directory. It enables `jev.simnow_cycle_test`,
sets a five-second evaluation interval and a 600-second runtime, and enables the
existing HTTP capture under `captures/`. It preserves the source risk settings
and never archives credentials or the full configuration. The application rejects
this Jev mode for other venues. Without the flag, the profit-oriented prompt is
unchanged. One contract and one instrument are enforced by the SimNow application.

The test prompt permits spread costs and losing trades without requiring a profit
edge. In this application's risk configuration, `max_loss: 0` disables the
monetary loss stop; it does not prohibit losing trades. It discourages adding to
an existing position and encourages closing it at a subsequent evaluation.

Review `summary.json`, `console.log`, component logs, and captured requests and
responses together. The summary counts alternating filled positions 0 -> +/-1 ->
0, not model choices or order submissions. Passing requires at least 20 cycles,
40 fill events, a final flat position, no provider failures, HTTP evidence and a
successful uninterrupted process. This is a chain-test result, not strategy PnL
evidence. Inspect HTTP metadata for capture write errors before accepting a run.

Holding for 5-15 seconds is a prompt target, not a timer or a measured acceptance
condition. Requests currently contain neither holding age nor session time
remaining. Final flat is an acceptance condition, not an automatic liquidation
guarantee: shutdown cancels outstanding orders but does not force-close holdings.
A run ending with a position fails and requires position reconciliation before
another run. Do not report unexecuted runs as meeting the frequency target.
