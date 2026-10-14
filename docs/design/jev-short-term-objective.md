# Short-term Jev decision objective

## Approved policy

Prioritize active trading for short-term profit over an intended holding horizon of 1–5 minutes. Favor opening on modest evidence when flat, adding exposure when favorable, and reducing exposure when exiting is preferable. When trading and holding appear similarly attractive, prefer the supported trading action. Reserve hold for opportunities outweighed by the bid–ask spread, binding risk limits, or insufficient information to support an action. The horizon is a planning target, not a timed exit rule: evaluation state has no entry timestamp.

Use the provider's choice without probability thresholds or local decision overrides. A directional preference alone does not justify opening. Reduce an existing position when exiting is preferable to continuing exposure, including at a loss. Retain the existing option sets and all open, hold, long, and short descriptions.

The 2026-10-14 revision changes only intent instructions; bias instructions and option descriptions retain their previous wording. Keep simex, evaluation state, risk checks, order sizing, execution, and provider failure semantics unchanged. This reversible policy adjustment does not establish that the market offers profitable opportunities or guarantee more trades.

## Validation

Run request contract tests and the full WSL Debug/Release cold builds and CTest suites. Tests cover the request structure and preserve flat versus occupied position options; they do not pin the full instruction wording.

A real-provider simex observation is bounded by 20 minutes or 60 provider calls, whichever occurs first. Record provider failures separately from HOLD. Use the unchanged simex configuration and a fresh participant session. Do not inject orders, override choices, or force liquidation at the observation limit.

Only an opening execution followed by closing execution and a return to zero position proves the complete trading cycle. OPEN without execution, an unclosed position, or HOLD-only results prove only the stages actually observed. The existing runner's any-execution success label is insufficient evidence of a complete cycle; inspect execution and position records.

## Deferred work

Temporal market context, entry timestamps, alternative simulated markets, and any new probability-based policy require a separate design decision. Do not tune wording repeatedly merely to obtain a fill.

## Observed result, 2026-09-27, before the active-trading revision

WSL GCC Debug and Release cold builds and CTest passed, 1/1 suite per configuration. A separate simex-enabled Debug cold build and CTest passed, 1/1.

The real-provider observation lasted approximately 512 seconds. It made 17 provider calls with no retries: 17 decisions were enqueued, 15 applied decisions were HOLD, and two decisions were dropped as stale during market-readiness transitions. There were zero provider failures and zero executions. The process reported `market_ready=1`, which records that usable depth was observed at least once, not that the market remained ready.

The observation was stopped early after several minutes without new evaluations because the market remained without usable two-sided depth. Trading exited cleanly with code 0. The temporary runner labels externally stopped trading as failed even on exit code 0; retain that raw result alongside this explanation. The 20-minute/60-call ceilings were not reached. Simex market parameters stayed unchanged; only its process lifetime was extended to cover the maximum observation window.

No complete trading cycle or increase in trading frequency was demonstrated. Raw logs are retained locally under `.scratch/short-term-validation/run-2/` in the task worktree. No orders were injected and no decisions were overridden. The initial launch failed before any provider calls because the build copy had no `.env`; the successful launch used the existing credential file through the program's working directory, without copying credentials.
