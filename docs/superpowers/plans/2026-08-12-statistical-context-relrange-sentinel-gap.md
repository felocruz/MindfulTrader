# Finding: `relative_range` Is Not Actually Fixed — Two Open Problems, Not One

Date: 2026-08-12
Status: **NOT FIXED** — recorded, not yet a task-by-task plan. Scope widened 2026-08-12 (same day) to
cover both problems below; they were investigated together because live data tied them to the same
`FeatureScaler` gap.
Scope: `src/TripleScreen2.cpp`, `include/FeatureScaler.h`

## Summary — Problem 1: `StatisticalContext.relRange` was never touched

`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` fixed dim 2
(`relative_range` in the exported `ObservationData` 16D vector) by replacing a raw `0.0f` sentinel
on degenerate ATR with `cfc::ComputeRelativeRange(...)` + carry-forward
(`TripleScreen2.cpp:273-274`). That fix is correct and verified against the spec.

A second, independent `relRange` calculation in the same file — `StatisticalContext.relRange`,
feeding `TrainingEvent.rel_range` via `ContextManager`'s ripple context, not the `ObservationData`
vector — has the identical bug shape and was never touched:

```cpp
// TripleScreen2.cpp:768-774
if (Array_AtrKeltner[sc.Index] > 0.0f) {
    float barRange = sc.High[sc.Index] - sc.Low[sc.Index];
    ctx.relRange = barRange / Array_AtrKeltner[sc.Index];
} else {
    ctx.relRange = 0.0f;   // same fabricated-neutral-zero pattern as pre-fix dim 2
}
```

## Why this was missed

Every audit run today (`2026-08-12-observation-vector-sentinel-collapse-audit.md`,
`2026-08-12-featurescaler-sentinel-collapse-hardening.md`, `2026-08-12-remaining-observation-vector-dims.md`,
`2026-08-12-tick-native-toxicity-illiquidity-design.md`, `2026-08-12-denali-data-feed-proxy-audit.md`) was
scoped to the exported `ObservationData`/16D-vector pipeline (`.context` files, `FeatureScaler`). None
mention `TrainingEvent`, `rel_range`, `StatisticalContext`, `WaveContext`, or `ripple` — grep-confirmed
zero hits across all five docs. This isn't a deliberate exclusion; the audits simply never looked at
`TrainingEvent`'s export path, so this sibling instance of the same defect class was never surfaced.

## Downstream relevance

Per CLAUDE.md, `StatisticalContext` (volatility, efficiency, **relRange**, velocity, regimeTenure) is
"the canonical mechanics backbone" and "must remain wired through TS2/TS3 → `ContextManager` →
`TrainingEvent`." Trace confirmed live, not dead code:

- `TripleScreen2.cpp:771/773` sets `ctx.relRange`
- `ContextManager.cpp:208` — `m_rippleContext.relRange = ctx.relRange;`
- `ContextManager.cpp:266` — `merged.relRange = m_rippleContext.relRange;`
- `ContextManager.cpp:622` — `event.rel_range = ripple ? ripple->relRange : 0.0f;`

`TrainingEvent.rel_range` is what `lbrnet` trains on directly — a fabricated exact-zero here on every
degenerate-ATR bar corrupts training data the same way the pre-fix dim 2 corrupted `.context` exports.

## Recommended fix shape (not yet implemented)

Same pattern already established and reviewed for dim 2: replace the raw `else { ctx.relRange = 0.0f; }`
branch with `cfc::ComputeRelativeRange(...)` (already exists, `CarryForwardCalculators.h`) fed by a
carry-forward persistent slot, mirroring `TripleScreen2.cpp:273-274` exactly. Needs its own persistent-var
slot (do not reuse `RELATIVE_RANGE_LAST_VALID_VALUE` — that one is dim 2's own carry-forward state and
these are two independently-updating consumers that can legitimately diverge tick-to-tick).

## Open question for whoever picks this up (Problem 1)

Whether `ctx.relRange` and dim 2's `Subgraph_RelativeRange` should actually be the **same value**
(one canonical `relRange` computation feeding both consumers) rather than two independently-computed,
now-diverging calculations of a supposedly identical quantity — that's a design question this finding
surfaces but does not answer. Worth a decision before writing the carry-forward fix, not just patching
the symptom twice.

---

## Problem 2: dim 2's own fix is deployed but the exported value is still ~63% exact-zero

Live evidence, gathered the same day this doc was written, against the currently-deployed DLL (SHA256-
verified identical to a fresh build of `f6f112d`/`941dfbe`, so this is not stale pre-fix data):
`context_preflight.py` (lbrnet, run read-only) sampled 400,000 aligned pairs from
`/mnt/c/SierraChart2/Data/event_data_20260812_195341.context`.

| dim | name | zero_ratio | read |
|---|---|---|---|
| 1 | `burstiness_index` | 71.4% | Essentially unchanged from its pre-fix baseline (~71-81%, per `2026-08-12-observation-vector-sentinel-collapse-audit.md` §2) despite the `e6b4003` carry-forward fix. |
| 2 | `relative_range` | 62.9% | Same story. |
| 8 | `fisher_info` | 12.9% | Same commit, same fix pattern — and this one *did* improve, matching the accepted warmup-only baseline (dims 0/6 sit at 10-13%). |

Dim 8's carry-forward worked; dims 1 and 2's did not move the needle. All three came out of the same
`e6b4003` commit with the same code shape (`cfc::Compute*(...)` + `sc.GetPersistentFloat` carry-forward),
so the difference isn't in *whether* carry-forward is wired — it's in what happens downstream.

### Root-cause hypothesis (not yet confirmed — needs investigation before a fix)

`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`'s **D2** — a log-only
diagnostic in `FeatureScaler::Calibrate()`/`Recalibrate()` for "a single raw value occupies >30% of a
dim's rolling window" — was never implemented (confirmed by reading `FeatureScaler.h:375-417` in full:
no value-frequency tracking exists there at all). That's exactly the mechanism that would explain this
data: carry-forward stops *fabricating* a sentinel, but if the degenerate branch still fires often, the
*repeated carried-forward value* can still dominate `FeatureScaler`'s rolling median just as effectively
— every recurrence of that repeated value still scales to an exact `z=0`. Carry-forward alone doesn't
close that loophole; it only changes what's being repeated.

This is a hypothesis, not a confirmed root cause — the `.context` file only exposes the *post-scaled*
value (per the sentinel-collapse audit's own scoping note), not the raw calculator output, so it isn't
possible to directly confirm from this data alone whether:
(a) `FeatureScaler` median-collapse on a repeated-but-nonzero carried-forward value is the mechanism, or
(b) the raw degenerate condition (`atr <= 0.00001f` for dim 2, i.e. `Array_ImpulseATR[sc.Index]` reading
near-zero) is itself firing far more often than expected — a different, upstream bug — or
(c) something else entirely.

Also relevant: **D3** (a `chronic_zero_threshold` gate in `context_preflight.py`, meant to catch
exactly this "chronically but not fully dead" pattern) was also never implemented — which is why this
run reports `status: PASS` with zero violations/warnings despite dims 1/2 sitting at 62-71% zero.

### Investigation tasks (root cause before fix, per `superpowers:systematic-debugging` —
matches this repo's own established pattern for dims 4 and 9 today)

- [ ] Confirm whether `Array_ImpulseATR[sc.Index] <= 0.00001f` is actually firing on ~63% of ticks (a
  real upstream ATR-warmup/data problem) vs. firing rarely while a *non-degenerate but repeated*
  `relRange` value is what's dominating the scaler's median instead.
- [ ] Same check for dim 1's `rv_older_rate < kFloor` branch (`CalculateBurstiness`,
  `StudyHelperFunctions.cpp`) — confirm which of the two mechanisms explains its 71.4%.
- [ ] If (b)/(c) — the scaler-side median-collapse hypothesis — build D2's single-value-dominance
  diagnostic first (it's cheap, log-only, and would directly confirm or rule out the hypothesis without
  guessing) before writing any fix.
- [ ] Only after root cause is confirmed: decide whether the fix belongs in the raw calculator (e.g. a
  wider ATR floor, or investigating why `Array_ImpulseATR` is degenerate so often) or in `FeatureScaler`
  itself (e.g. D2's diagnostic plus a policy for what to do when it fires), or both.
- [ ] D3 (`chronic_zero_threshold` in `context_preflight.py`) should land regardless of root cause — it's
  the regression net that would have caught this before it shipped, and would catch a recurrence.
