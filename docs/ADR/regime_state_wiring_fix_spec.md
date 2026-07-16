# Spec — Regime-State Wiring Fix (Execution Correctness Finding 1)

**Status:** IMPLEMENTED (2026-07-14, commit `097e11b`; re-verified 2026-07-15). `SyncRegimeState()` extracted + wired at the top of `Update()`; `UpdateContext()` split (sync + defense); single per-tick `EvaluateRegimeDefense`. Build green.
**Scope:** C++ execution layer only. **Schema-free** (no `mts_schema.fbs` change; consistent with the current schema-deferral directive).
**Resolves:** `docs/ADR/execution_correctness_findings_spec.md` Finding 1 (CRITICAL) and its addendum.
**Prerequisite for:** Triple-Barrier Exit Engine Phase 1 (the regime-conditioned vertical barrier / kill-switch reads this same state — see `triple_barrier_exit_engine_spec.md` §4.3, §7).

---

## 1. Problem (verified against working tree, 2026-07-14)

`PositionManager::UpdateContext(SCStudyInterfaceRef)` — declared `include/PositionManager.h:97`,
defined `src/PositionManagerPatterns.cpp:55-72` — is the **only** writer of the four regime-state
members:

```cpp
void PositionManager::UpdateContext(SCStudyInterfaceRef sc) {
    auto* hmmInd = InferenceManager::Instance().HmmState();
    if (hmmInd) { m_previousHMMState = m_currentHMMState; m_currentHMMState = hmmInd->Value(); }
    auto* climateInd = InferenceManager::Instance().MarketClimate();
    if (climateInd) { m_previousClimate = m_currentClimate; m_currentClimate = climateInd->Value(); }
    if (!IsFlat()) { EvaluateRegimeDefense(sc); }   // <-- also does defense
}
```

A repo-wide grep confirms **zero call sites** of `.UpdateContext(`. Therefore
`m_currentHMMState`/`m_currentClimate`/`m_previousHMMState`/`m_previousClimate` remain frozen at
their default-constructed values (`HMM_NO_PRIOR` / `MarketClimate::GAUSSIAN_STABLE`) for the entire
process lifetime.

### 1.1 Consumers running on frozen state (both inside `Update()`'s `!IsFlat()` block)

| Consumer | Location | Effect of frozen state |
|---|---|---|
| `EvaluateRegimeDefense()` | `PositionManager.cpp:236` | GAP 8 hostile-regime exit never fires (`m_current == m_previous == HMM_NO_PRIOR`); GAP 5 climate-shift trailing gate never activates; holding-score toxic/hostile multiplier always uses the default. |
| `UpdateTradeGradeProtection()` | `PositionManager.cpp:233 → 3347` | `GetRegimeGradeThresholds(m_currentHMMState, m_currentClimate)` always hits the `default:` branch → base 30/20/10 Elder-grade thresholds regardless of live regime. |

### 1.2 Call-flow facts

- Live: `SCStudies.cpp` — `CheckAndTriggerHMM(...)` (refreshes the HMM/climate **indicators**) precedes `PositionManager::Update(sc)` at `:418`.
- Backtest: `BackTesterStudy.cpp` — `CheckAndTriggerHMM(...)` at `:916` precedes `PositionManager::Update(sc)` at `:923`.
- In **both** paths the indicators are fresh at the moment `Update()` runs; only the copy into `PositionManager` members is missing.

---

## 2. Why the literal Finding-1 fix is rejected

Calling `PositionManager::Instance().UpdateContext(sc)` from the SCStudies orchestrator (as the
finding suggested) is defective:

1. **Double-evaluation.** `UpdateContext()` calls `EvaluateRegimeDefense()`, and `Update()` calls it
   again at `:236` → regime defense fires twice per tick in-position (double logging; risk of
   double-acting exit/trailing gates).
2. **Backtest-parity break.** Wiring only SCStudies leaves `BackTesterStudy.cpp:923`'s `Update()` on
   frozen state → live/backtest divergence, violating the Backtesting Governance Contract. Duplicating
   the call in both studies is fragile and drift-prone.
3. **Ordering.** `UpdateTradeGradeProtection()` (`:233`) consumes the frozen fields and runs *before*
   `EvaluateRegimeDefense()` (`:236`). The refresh must precede the entire `!IsFlat()` consumer block,
   not just the defense call.

---

## 3. Design — refresh inside `Update()`, defense stays single-shot

Split the two responsibilities currently fused in `UpdateContext()`:

- **State refresh** (read the two indicators into `m_previous*`/`m_current*`) → extract into a private
  `SyncRegimeState()`.
- **Defense evaluation** (`EvaluateRegimeDefense`) → stays exactly where it is (`Update()` `:236`),
  now consuming fresh state, fired **once** per tick.

`UpdateContext()` is preserved as the SystemOrchestrator event-driven entry point, refactored to
`SyncRegimeState()` + `EvaluateRegimeDefense()` so its semantics are unchanged but it shares the one
refresh implementation.

Because the refresh lives inside `Update()`, **both** the live (`SCStudies`) and backtest
(`BackTesterStudy`) paths are fixed uniformly with a single edit — no call-site duplication, no
parity gap.

### 3.1 Changes

**`include/PositionManager.h`** — add private declaration (near line 97, beside `UpdateContext`):

```cpp
    // Copies the live HMM + MarketClimate indicator values into
    // m_previous*/m_current*. Pure state sync — no side effects. Called at the
    // top of Update() every tick so all in-position consumers
    // (UpdateTradeGradeProtection, EvaluateRegimeDefense) read fresh regime.
    void SyncRegimeState();
```

**`src/PositionManagerPatterns.cpp`** — extract the refresh; rewire `UpdateContext()`:

```cpp
void PositionManager::SyncRegimeState() {
    if (auto* hmmInd = InferenceManager::Instance().HmmState()) {
        m_previousHMMState = m_currentHMMState;
        m_currentHMMState  = hmmInd->Value();
    }
    if (auto* climateInd = InferenceManager::Instance().MarketClimate()) {
        m_previousClimate = m_currentClimate;
        m_currentClimate  = climateInd->Value();
    }
}

// Event-driven entry point (SystemOrchestrator on regime-change). Retained so an
// out-of-band regime event can force an immediate defense pass; shares the one
// refresh implementation. NOT called from the per-tick Update() path (that uses
// SyncRegimeState directly to avoid a double EvaluateRegimeDefense).
void PositionManager::UpdateContext(SCStudyInterfaceRef sc) {
    SyncRegimeState();
    if (!IsFlat()) { EvaluateRegimeDefense(sc); }
}
```

**`src/PositionManager.cpp`** — refresh at the top of `Update()`, right after `CachePreviousState(sc)`:

```cpp
void PositionManager::Update(SCStudyInterfaceRef sc) {
    CachePreviousState(sc);

    // Finding 1 fix: refresh regime state every tick BEFORE any in-position
    // consumer (UpdateTradeGradeProtection @ 233, EvaluateRegimeDefense @ 236).
    // Indicators are already refreshed by ContextManager::CheckAndTriggerHMM(),
    // which precedes Update() in both SCStudies and BackTesterStudy.
    SyncRegimeState();

    // ... existing body unchanged; EvaluateRegimeDefense(sc) remains the single
    //     per-tick defense call inside the !IsFlat() block ...
}
```

No change to `SCStudies.cpp` or `BackTesterStudy.cpp`.

---

## 4. Correctness argument

- **Freshness:** `CheckAndTriggerHMM()` precedes `Update()` in both paths ⇒ `SyncRegimeState()` reads
  current-tick indicator values.
- **Single evaluation:** `EvaluateRegimeDefense()` is called exactly once per tick (still at `:236`);
  the top-of-`Update()` refresh does no defense.
- **Ordering:** refresh runs before `UpdateTradeGradeProtection()` (`:233`) and
  `EvaluateRegimeDefense()` (`:236`) ⇒ both see live regime.
- **Change detection:** `m_previous*`/`m_current*` transition semantics preserved. On no-change ticks
  `m_previous == m_current` (harmless; GAP-8 requires `!= && m_previous != HMM_NO_PRIOR`). On the
  tick the HMM flips, exactly one `m_previous → m_current` transition is observed.
- **Parity:** identical behavior in live and backtest — one implementation, one call site.

---

## 5. Verification / acceptance

1. Build green via `./build_dll.sh`.
2. Add a temporary trace in `EvaluateRegimeDefense()` (or assert) confirming
   `m_currentHMMState` changes over a replay (previously constant `HMM_NO_PRIOR`).
3. Replay a session that crosses a regime boundary; confirm a `HOSTILE_REGIME_EXIT` /
   climate-shift trailing activation can now fire (previously impossible).
4. Confirm `EvaluateRegimeDefense` runs once per tick (no double log lines).
5. `.btst` attribution: grade thresholds now vary by regime (not always 30/20/10).

---

## 6. Open verification item (second-order)

Audit whether any **entry-time** path (scoring / `ProcessPendingPrediction`) reads
`m_currentHMMState`/`m_currentClimate` rather than the indicators directly. Known consumers
(`PositionManager.cpp:511`, `:3144`) read `climateInd->Value()` **directly** and are unaffected. If a
frozen-member read is found on the entry path, it is covered by the same `SyncRegimeState()` refresh
(now fresh before `ProcessPendingPrediction`, which runs after the top-of-`Update()` refresh).
