# Short-term Jev decision objective

## Approved policy

Seek short-term profit over an intended holding horizon of 1–5 minutes. Modest opportunities supported by limited information are sufficient; strong evidence is not required. Consider the bid–ask spread. Hold when no worthwhile opportunity is apparent. The horizon is a planning target, not a timed exit rule: evaluation state has no entry timestamp.

Use the provider's choice without probability thresholds or local decision overrides. A directional preference alone does not justify opening. Reduce an existing position when exiting is preferable to continuing exposure, including at a loss. Retain the existing option sets and all open, hold, long, and short descriptions.

Only bias instructions, intent instructions, and the close description change. Keep simex, evaluation state, risk checks, order sizing, execution, and provider failure semantics unchanged. This reversible policy adjustment does not establish that the market offers profitable opportunities or guarantee more trades.

## Validation

Run request contract tests and the full WSL Debug/Release cold builds and CTest suites. Tests pin the approved English wording and preserve flat versus occupied position options.

A real-provider simex observation is bounded by 20 minutes or 60 provider calls, whichever occurs first. Record provider failures separately from HOLD. Use the unchanged simex configuration and a fresh participant session. Do not inject orders, override choices, or force liquidation at the observation limit.

Only an opening execution followed by closing execution and a return to zero position proves the complete trading cycle. OPEN without execution, an unclosed position, or HOLD-only results prove only the stages actually observed. The existing runner's any-execution success label is insufficient evidence of a complete cycle; inspect execution and position records.

## Deferred work

Temporal market context, entry timestamps, alternative simulated markets, and any new probability-based policy require a separate design decision. Do not tune wording repeatedly merely to obtain a fill.
