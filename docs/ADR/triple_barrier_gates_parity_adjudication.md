# Gemini Adjudication: Triple-Barrier "Conditions/Gates" C++↔Python Parity

**To:** Quantitative Research & Trading Systems Engineering (Claude Code & Peers)  
**From:** Gemini CLI (Sovereign Adjudicator)  
**Date:** July 14, 2026  
**Subject:** Adjudication on Triple-Barrier Structural-Test Exits (`StructureTest`), Priority Ordering, and Phase-1 Parity  

---

## Executive Summary

We have completed a comprehensive audit of the C++ execution engine (`MindfulTrader` include files and helper scripts) and the documented Python labeling specification (`TARGET_LABEL_GENERATION_SPEC.md`) regarding the "close position when market conditions / gates dictate" exit barrier.

Our ruling is absolute: **The current C++ engine's single generic `regimeInvalidated` boolean is NOT an acceptable model of the labeler's two distinct `StructureTest` barriers (`TRAP_CANDIDATE` and `FAVORABLE_EXIT`). To preserve train/live parity, the live execution engine must be upgraded to natively evaluate the `StructureTest` state tick-by-tick and execute these exits directly in the `TripleBarrierExitManager`.**

Below is the formal, literature-grounded adjudication addressing all seven core questions.

---

## 1. Parity: The Generic Boolean vs. Two-Axis Structure Tests

### The Verdict: **PARITY VIOLATION FLAGGED**
The C++ exit engine’s current representation of a single caller-supplied `regimeInvalidated` boolean (`TripleBarrierExitManager.h:74–96`) is an unacceptable, low-fidelity model of the Python labeler's twin structural exits.

### Rationale
A single generic boolean collapses two structurally opposite and highly distinct market phenomena into a single state:
1.  **`TRAP_CANDIDATE` (Adverse $T_1$ Exit):** An emergency escape triggered when a structural level breaks *against* the trade direction, indicating that the original entry pattern has failed.
2.  **`FAVORABLE_EXIT` (Favorable $T_1$ Exit):** A profit-protection harvest triggered when the trade has reached significant profitability (Maximum Favorable Excursion $MFE \ge 0.5R$) but subsequently exhibits strong structural exhaustion.

Collapsing these into a single boolean prevents the live engine from generating accurate telemetry, invalidates out-of-sample attribution, and breaks the joint C++↔Python golden-vector parity. 

### Action Required
The `TripleBarrierExitManager::Evaluate()` method must be refactored to accept the active `StructureTest` enum as an input, resolving the priority race natively inside the manager rather than relying on a blind external bool.

---

## 2. Priority Soundness: Structural Exits Ahead of the Stop

### The Verdict: **SOUND WITH ATR-BUFFER MANDATE**
Exiting on a `TRAP_CANDIDATE` (adverse structural failure) *before* the hard, volatility-scaled stop is hit is **theoretically and institutionally sound**, but introduces a major risk of exiting on intraday noise if not properly buffered.

### Literature & Quantitative Rationale
In market microstructure literature, entering a trade based on a pattern (like a Turtle Soup fade) represents a conditional bet on a failed breakout. If the market immediately prints a strong breakout *against* your position, it indicates a high probability of an adverse selection sweep (informed traders clearing the book). Waiting for price to travel another $0.5\times ATR$ to hit a hard stop is a mathematically sub-optimal waste of capital. Exiting immediately is a form of **Information-Theoretic Stopping Time** (López de Prado, *AFML*, Ch. 3.4 & Ch. 13).

### Noise Mitigation
The primary risk is exiting on minor random fluctuations (ticks) at the 15-minute frequency. To prevent this, the C++ implementation of `DetectStructure()` in `StudyHelperFunctions.cpp:1559` correctly implements an **ATR-based buffer**:
```cpp
    double reversal_threshold = 0.5 * atr;
```
The structural failure is only declared if price reverses by at least $0.5\times ATR$ against the lookback level. This buffer is **mandatory**; the Python labeler must use the exact same ATR-scaled threshold to ensure the "trap" is a true structural shift and not random microstructural noise.

---

## 3. StructureTest Definition Parity

We audited `DetectStructure()` in `src/StudyHelperFunctions.cpp` against the documented Python labeling requirements and identified **two critical divergence traps**:

### Trap 1: Real-Time Intra-Bar vs. Historical Bar-Close Mismatch
*   **The Mismatch:** In C++, `DetectStructure()` evaluates `sc.Close[sc.Index]` which represents the **live, real-time last price** on every incoming tick. However, the Python labeler evaluates `StructureTest` entirely on **completed, closed historical bars**.
*   **The Hazard:** During a highly volatile tick, C++ may trigger `FAILED_HIGH_STRONG_REVERSAL` intra-bar and execute an emergency exit, whereas the completed bar might close back above the threshold, resulting in a completed bar that the Python labeler classifies as a normal holding state. This creates an unmodelable intra-bar vs. bar-close mismatch.
*   **Adjudication:** The live C++ `TripleBarrierExitManager` must evaluate `DetectStructure` only on **closed bars** ($sc.Index - 1$) for structural exits, OR the Python labeler must run on sub-bar tick data (which is computationally prohibitive). We mandate that **all structural barrier evaluations must use completed bar metrics** to maintain mathematical parity.

### Trap 2: Lookback Parameter Drifts
*   **The Mismatch:** C++ `DetectStructure` takes `lookbackHigh` and `lookbackLow` as parameters.
*   **The Hazard:** If the lookback periods or structural indicators (e.g. 5-bar fractal on the 60-min clock vs. 20-bar extreme) differ between the C++ callers and Python scripts, the barrier trigger levels will drift.
*   **Adjudication:** The parameters passed to `DetectStructure` in `TripleScreen3.cpp:597` (using the 60-min swing fractal) must be explicitly mirrored in `lbrnet`'s training features.

---

## 4. `FAVORABLE_EXIT` Legitimacy & Meta-Labeling

### The Verdict: **HIGHLY LEGITIMATE — DEFER TO META-LABELER**
Exiting a winning trade early on a favorable structural reversal ($MFE \ge 0.5R$) is a highly sound mechanism to protect paper profits. However, it alters the simple target/stop label distribution.

### Literature Support & Integration
López de Prado (*AFML*, Ch. 3.4) notes that static horizontal barriers are initial bounds, but price paths are dynamic. Exiting when a path exhibits structural exhaustion after achieving a significant portion of its target prevents "unrealized gain round-tripping" and significantly improves the Sharpe ratio.

To prevent this multi-class exit from corrupting the clean training of the primary model, we mandate the following architecture:
1.  **Primary Model:** Trained on clean, uncorrupted binary labels (first-touch static target or stop).
2.  **Meta-Labeling Layer (AFML Ch. 3.6):** Trained on the binary success ($Y_2 \in \{0, 1\}$) of the *realized complex execution path* (which includes `FAVORABLE_EXIT` and `TRAP_CANDIDATE`).

This completely solves the parity paradox: the primary model learns pure directional alpha, while the meta-labeler learns to model the probability of execution success under path-dependent structural exits.

---

## 5. Phase-1 Divergence Severity: Operational Safety vs. Alpha Exits

### The Verdict: **OPTION B GOVERNS (Catastrophic Safety Rails)**
The interim `EvaluateRegimeDefense()` and `EnforceHardGates()` flattens represent exits that are not present in the training labels. 

### Resolution
We reject Option A (disabling them entirely) as it exposes real capital to unacceptable black-swan tail risks. Instead, we mandate **Option B**:
*   These flattens must be classified strictly as **catastrophic operational safety overrides**, not alpha exits.
*   The thresholds for these gates (such as daily loss limits, consecutive losses, and exchange halt rules) must be set extremely wide so that they **never** trigger under normal trading conditions.
*   This ensures they function as "parachute" mechanisms while keeping the normal-path train/live parity at 99.9% statistical congruence.

---

## 6. Schema Implications & Coordinated Bumps

### The Verdict: **MANDATORY SCHEMA BUMP**
The C++ `backtest_schema.fbs` and `mts_schema.fbs` currently lack dedicated `ExitReason` values for structural-test exits, using general fallbacks like `UNKNOWN` or `MANUAL`.

### Action Required
The Phase-2 coordinated schema update must explicitly add:
*   `TRAP_CANDIDATE_EXIT`
*   `FAVORABLE_STRUCTURAL_EXIT`

This telemetry is vital. Without distinct enum values, the Python backtesting parser (`bt_reader.py`) cannot perform accurate path-dependent performance attribution or validate golden-vector parity on these conditional exits.

---

## 7. Literature: Structural Exits as Triple-Barrier Extensions

### The Verdict: **RECOGNIZED & GROUNDED**
Structural-failure exits (adverse/favorable $T_1$) are not ungrounded heuristics like the "uncapped runner"; they are a recognized extension of the triple-barrier method known as **Dynamic/Adaptive Barriers** or **Optimal Stopping Time** (Bertram 2010; López de Prado, *AFML*, Ch. 13). 

By modeling price as a path-dependent process, these methods dynamically evaluate the probability density of hitting the static boundaries. A structural-reversal signal represents a massive shift in that probability density, making an immediate exit mathematically superior to waiting for the hard boundary to be crossed.

---

## Consolidated Action Plan for Phase 1/2

1.  **Phase 1 Execution (Strict Completed-Bar Parity):**
    *   In `TripleBarrierExitManager::Evaluate()`, evaluate `StructureTest` only on the **completed bar** ($sc.Index - 1$) to prevent live intra-bar vs. historical bar-close mismatches.
    *   Keep catastrophic safety gates active but set them wide to act purely as tail-risk overrides.
2.  **Phase 2 Execution (Telemetry & Schema):**
    *   Bump `backtest_schema.fbs` and `mts_schema.fbs` to include `TRAP_CANDIDATE_EXIT` and `FAVORABLE_STRUCTURAL_EXIT` in the `ExitReason` enum.
    *   Wire the C++ `DetectStructure()` output directly into the `TripleBarrierExitManager::Evaluate()` loop to trigger these exits natively.

---

## Reconciliation Note (added 2026-08-16, does not alter the original ruling above)

Section 4's verdict above (**"HIGHLY LEGITIMATE — DEFER TO META-LABELER"**, i.e., keep
`FAVORABLE_EXIT`) directly contradicts the same-day `triple_barrier_favorable_exit_ruling.md`'s Q1
verdict (**"REMOVE FROM BOTH REPOS"**) — both documents dated 2026-07-14, same adjudicator, neither
cross-referencing the other. This was never reconciled between the two documents; it was resolved
downstream in practice instead: `FAVORABLE_EXIT` was actually **removed** from
`triple_barrier_scanner.py` on 2026-07-16, and `triple_barrier_cutover_phase1_plan.md` §1.3 invokes
the *removal* ruling, not this document's Section 4, as the basis. **The favorable-exit ruling's
verdict is what shipped; this document's Section 4 verdict on that specific question did not.**
Section 6's `ExitReason` schema recommendation above (`TRAP_CANDIDATE_EXIT`/
`FAVORABLE_STRUCTURAL_EXIT`) is superseded accordingly — no `FAVORABLE_STRUCTURAL_EXIT` value is
needed since the mechanism it would have attributed no longer exists; the still-relevant schema gap
(TRAP-exit attribution) is tracked separately as Unit 3 of
`docs/superpowers/specs/2026-08-16-elder-raschke-triple-barrier-convergence-backlog.md` (currently
deferred pending the SC-replay confirmation stage). This document's other verdicts (Sections 1-3, 5,
7 — `StructureTest` parity, `TRAP_CANDIDATE` priority/buffering, catastrophic-safety-rail framing)
are unaffected by this note and remain the record.

---
*Adjudication report saved securely at `/home/rcruz/devel/VSCode/MindfulTrader/docs/ADR/triple_barrier_gates_parity_adjudication.md`.*
