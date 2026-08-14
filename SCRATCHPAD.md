# Session Scratchpad — Where We Left Off

Last updated: 2026-08-14 17:10 (session end)

## What's currently running

An overnight EventDataCollector Phase 1 replay is live:
- Log: `/mnt/c/Trading/logs/MindfulTrader.log`
- Output: `/mnt/c/SierraChart2/Data/event_data_20260814_163135.context` (+ `.alpha` sibling)
- Started (Export armed / hard reset): 2026-08-14 16:31:35
- `LockA` unlocked (Alpha collection active): 2026-08-14 16:47:32 — took 15m57s wall-clock,
  which corresponds to ~19.3 days of TS1 (240-min bar) timeframe warmup to reach
  `macro_window=100`. This is expected/reasonable, not a bug — see "LockA audit" below.
- Replayed market dates so far: ~2023-08-16 through ~2023-09-04 (NOT current dates — this is
  a historical replay, not live/2026 data). File was still growing at 121MB as of 17:10:44,
  last check showed no stalls, no errors, healthy digest counters.

**Next step if picking this back up**: check current log tail / file size, and once the replay
has advanced further into September 2023 (the date range `dim6`'s 14.8%-tail derivation was
based on), re-run the `.context` preflight analysis (see below) sampling further into the file
to see whether `dim6_hurst_exponent` starts showing exceedances near its 345.0 bound — the
first-50K-rows sample only saw max ±5.846, well short of both the old 6.0 and new 345.0
thresholds, plausibly just because this early slice hadn't reached the regime-shift-heavy
window yet.

## LockA audit — resolved, don't re-investigate

Traced `EventDataCollectorStudy.cpp`'s `LockA` gate (`ContextManager::IsObservationSaturated()`
→ `FeatureScaler.warmedUp`/`sampleCount`, 500-sample threshold) down to `AreTs1DimsReady()`
requiring `macro_window=100` TS1 bars. Confirmed via full timestamped `idx=` trajectory (not
just point samples) that TS1 was steadily advancing the whole time — ~1 bar/8-10 real seconds —
not stuck. Once it crossed 100 bars, `LockA`'s 500-sample requirement resolved in under 1 second
(replay throughput is very high once unblocked). **Conclusion: working as designed, no defect.**
100 bars is already the literature-minimum this session's own Gang-doc audit flagged as
`under-powered` for DFA/Hurst (Weron 2002 doesn't characterize below N=256) — no slack to shrink
this warmup further without trading away estimator reliability.

## `.context` preflight findings (first 50,000 aligned pairs only — file is much bigger now)

Confirmed via source (`ContextManager.cpp:895-926`, `MakeObservationData(currentObs)`) that
`.context` stores the **FeatureScaler-scaled output**, not raw physics values — so this data
directly exercises today's winsorization/shrinkage work.

- **D1 sentinel-collapse fix confirmed working on real data**: dims 1/2/7/11 (originally 40-80%
  exact-zero incident) now show 0.16% / 0.03% / 0.22% / 2.83% zero-ratio respectively. Clean
  validation outside of unit tests.
- **dim4's new 12.0 winsorization bound is live and engaging**: sampled max = 12.000 exactly.
- **dim2 hit its flat -6.0 default bound exactly** in tick-level replay — a small honest
  correction to today's earlier audit, which closed dim2 as "0 exceedances, no tick-level
  replica needed" based on a bar-close-only historical screen. Not urgent (winsorization caught
  it correctly), but the "closed clean" characterization was slightly optimistic for tick-level
  behavior specifically. Worth a note if `dim2`'s Gang-doc/FeatureScaler.h comment is ever
  revisited — not filed as an open task, just a documented observation.
- **dim6 has not yet shown its heavy tail** in this sample — max ±5.846, nowhere near either the
  old 6.0 or new 345.0 bound. Not contradictory (see "next step" above), just unresolved by this
  slice of data.
- Zero violations, zero warnings, correlation matrix clean (max abs corr 0.59, nothing >0.8).
- Full JSON report from this run is scratch-only, not saved to the repo (was written to a
  session tmp path, not durable) — re-run `mamba run -n mts python scripts/context_preflight.py
  --input <path> --report-json <out>` from `lbrnet/` if this exact analysis needs reproducing.

## Uncommitted, intentionally left as-is

- `data/NH_NL.csv` and `data/daily_high_low.csv` — both refreshed through 2026-08-13 real data
  (NH-NL from user-supplied StockCharts export, daily high/low via
  `populate_daily_high_low_hybrid.py --start-date 2026-08-07`), both mirrored to
  `/mnt/c/Trading/data/`. User explicitly said "leave it" (uncommitted) — this is expected repo
  state, not stray work to investigate or commit unprompted.

## This session's completed work (all committed)

1. `f7c47bf` — fixed a real `DIM_WINSOR_SIGMA_OVERRIDE[0]`/`[9]` transposition bug in
   `FeatureScaler.h`, caught while cross-checking Gang-doc numbers directly against shipped code
   rather than trusting derivation notes. TDD regression test added.
2. `86dfd25` — Gang-doc entries for the full 16D observation-vector audit (raw clamp guardrail,
   Weibull vs. Fréchet winsorization bounds, shrinkage-as-Ledoit-Wolf-synthesis, new Mandelbrot-
   pillar `dim6` memory-clustering finding).
3. `b1c5ddb` / `3050fdf` — Task 5/6 of
   `docs/superpowers/plans/2026-08-14-observation-vector-full-institutional-coverage.md` marked
   done/flagged. Task 6 items (lbrnet D3 gate, `/mnt/c/Trading/config/` drift) are cross-repo
   pointers, not implemented from here by design.
4. `26bb5f9` — `docs/ROADMAP_EXECUTION_ENGINE.md` audited and marked **SUPERSEDED**: all four
   proposed upgrades already exist, mostly via more sophisticated mechanisms; one (time-decay
   exit) was implemented and deliberately removed for Triple-Barrier train/live parity, so
   re-implementing it would be a regression. Synced across all 4 Documentation Sync Contract
   mirrors plus `docs/CLAUDE.md`.
5. `lbrnet` repo (separate session boundary respected): committed
   `docs/superpowers/specs/2026-08-14-context-preflight-chronic-zero-gate-spec.md` (commit
   `87d4ec7`) — the D3 task brief, not yet implemented, meant to be picked up from an
   lbrnet-rooted session.

## Explicitly deferred, still open

- **Task 1** (production deploy/validation) — now actually IN PROGRESS via the overnight replay
  above, but not yet concluded/signed off. Revisit once the replay has run further.
- **lbrnet D3** (`context_preflight.py` chronic-zero gate) — spec written, not implemented;
  pick up from an lbrnet-rooted session.
- **Config drift** at `/mnt/c/Trading/config/` (two live JSON files with pre-D1-D9 thresholds) —
  flagged, not touched; needs explicit user sign-off before editing.
- **`config_hash` audit-event governance** (`TRADE_EXECUTION_SYSTEM.md` §14.2) — discussed in
  depth (see conversation), scoped down to hashing `ExecutionParams::LoadFromFile()`'s /
  `RiskManager.cpp`'s already-in-memory `payload` string and logging via the existing `Logger`
  call site — not started.
- **Volume Profile proxy replacement** (`docs/ADR/sierra_chart_data_feed_setup.md`) — identified
  as a good next quant-value candidate, not started.
