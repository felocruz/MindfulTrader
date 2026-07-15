# Spec — `RiskGateContext` Wire Serialization (Co-Evolution)

**Status:** SPEC — DRAFT FOR REVIEW (2026-07-14). Schema-touching; **deliberate coordinated both-sides bump.**
**Resolves:** MindfulTrader Findings 19/20/21 = lbrnet PC-15/16/17 (raw-vs-scaled gate-signal mismatch).
**Co-evolution:** neither side authoritative by default. C++ defines the institutional home; Python is instructed to read from it; both land in one bump; golden-path validation confirms parity.
**Sequencing:** should land **before** the Triple-Barrier cutover's `omega_net`-gated acceptance — the Python backtester (the measurement instrument) currently checks gates on scaled proxies with empirical stopgaps, so the baseline is contaminated by exactly these signals until this ships.

---

## 1. Problem

The C++ risk gates (`RiskManager`) evaluate `LocalRiskContext` (`ContextManager.h:67`) — the single source of truth for gate decisions. **None of it is serialized.** Only PHASE-2A-scaled projections of a few fields leak into the 16-D `ObservationData` vector (log-z + winsorize / SOFTLOGZ). The raw values exist only in-process.

The Python backtester **simulates C++ execution** (`backtest_runner.py`, Phase 2 live-sim). Because it can't see the raw gate inputs, it reads the scaled obs and applies **empirical stopgap thresholds**:

| Python | C++ Finding | Raw signal (never on wire) | `LocalRiskContext` field | Live gate |
|---|---|---|---|---|
| PC-15 | 19 | `liq_fragility` raw `[0,1]` | `spreadStress` | `> 0.85` |
| PC-16 | 20 | `tail_index` raw Hill-α | `paretoTailAlpha` | Pareto breach |
| PC-17 | 21 | `vpin_toxicity` raw Amihud | `vpin` | `> 0.80 / 0.40` |

All three: *"TEMPORARY … remove once the C++ side ships the raw signal on the wire."*

## 2. Data-flow facts (verified 2026-07-14)

- **Live agent:** C++ → `Event` (FlatBuffer, `EventBuilder`) → ZMQ → Python live agent (model/decisions).
- **Live HMM:** `HMMClient` (5561 DEALER) builds **`MarketObservation`** (16-D `ObservationData` only — `asymmetry_context` explicitly omitted: *"Transformer-Only via Event stream"*, `HMMClient.cpp:416`) **+ `SystemState`**, sent as an atomic 4-frame multipart (`SendBinaryRequest`).
- **Offline `.alpha`:** stream of `TrainingEvent` — read by the backtester dual-iterator, training, HPO, labeler.
- **Offline `.context`:** alternating **`MarketObservation`/`SystemState`**, emitted by `ContextManager::EmitTrainingContext(currentObs, asymContext, now_us)` (`ContextManager.h:888`). The backtester reads gate inputs from `MarketObservation.Observation()`.
- **Key structural facts driving placement:**
  1. `MarketObservation` is the **same table** live (HMM) and offline (`.context`) — no live/offline table divergence (unlike `Event`↔`TrainingEvent`, which need the paired shared-writer parity mechanism).
  2. It is **exactly where the offline sim already reads gate inputs** (`MarketObservation.Observation()`), so the raw twin lands beside the scaled value it replaces — zero dual-iterator cross-referencing.
  3. `ContextManager` **owns both** the `.context` emit (`EmitTrainingContext`) **and** `m_localRiskContext` — the population site and the data source are in one class; no cross-module plumbing.
- `AsymmetryContext` is a **struct** (fixed 8 floats) → cannot be extended additively (rejected as a home for that reason).

## 3. Design decision — a first-class `RiskGateContext` table

Serialize `LocalRiskContext` as a **new `RiskGateContext` table** (not loose `raw_*` scalars, not inside the `AsymmetryContext` struct). It completes the role split:

- `observation` (`ObservationData`) → HMM/model input (scaled)
- `asymmetry_context` (`AsymmetryContext`) → transformer embedding (raw-ish)
- **`risk_gate_context` (`RiskGateContext`) → risk-gate input (raw, unscaled)** ← new

Rationale:
- **Total parity** — Python reads the *exact* values C++ gates check; covers all gates, not just 19/20/21.
- **Future-proof / DRY** — new gate signals added to `LocalRiskContext` flow once the writer is wired; single source stays single.
- **Table → additive-with-defaults → non-breaking**; existing `.alpha`/`.context` keep parsing. (A struct extension would be a breaking layout change + a model-embedding-width risk — rejected.)
- **Semantically honest & versioned** — the gate-decision context as its own named wire contract.

## 4. Schema changes (`../schema/mts_schema.fbs`)

**New table** (mirror `LocalRiskContext` 1:1 — serialize the full struct for a complete, future-proof gate context):

```fbs
// Raw, UNSCALED risk-gate inputs — the exact LocalRiskContext values the C++
// RiskManager hard gates evaluate (ContextManager.h). NOT scaled (unlike
// ObservationData). Enables faithful Python execution-sim gate parity.
table RiskGateContext {
  shannon_flow_entropy : float = 0.0;   // bits
  shannon_efficiency   : float = 0.5;   // 1 - H/Hmax
  taleb_kurtosis       : float = 0.0;
  taleb_skewness       : float = 0.0;
  elder_chandelier_atr : float = 0.0;
  pareto_tail_alpha    : float = 4.0;   // Finding 20 — raw Hill-α
  vpin                 : float = 0.0;   // Finding 21 — raw Amihud (legacy name kept; see PC-03)
  spread_stress        : float = 0.0;   // Finding 19 — raw [0,1] fragility
  hurst_exponent       : float = 0.5;
  fractal_dim          : float = 1.5;
  mean_rev_z           : float = 0.0;
  raschke_burst        : float = 1.0;
  fisher_info          : float = 0.0;
  regime_duration      : int   = 0;
  is_valid             : bool  = false;
  snapshot_timestamp_us: long  = 0;
}
```

**Embed additively on `MarketObservation` only** (append at end of table):
- `MarketObservation`: `risk_gate_context : RiskGateContext;`

Rationale for the single home (vs. the earlier `Event`+`TrainingEvent` draft): `MarketObservation` is the same table live+offline, is where the sim already reads gates, and co-locates the raw twin with the scaled obs. `Event`/`TrainingEvent` are **not** changed — the offline sim does not read gate inputs from `.alpha`, and live gating is enforced C++-side (the live agent does not replicate gates). Adding it to `TrainingEvent` later is a separate, optional decision *only* if training wants the raw signals as model features.

Legacy-name note: keep `vpin` (documented as Amihud, PC-03) to avoid a rename's cross-repo blast radius; PC-03's rename is a separate deferred decision.

## 5. C++ wiring

- **Source:** the in-process `m_localRiskContext` (`ContextManager` owns it) — the **raw** values, read *before* PHASE-2A `FeatureScaler`. No recomputation, no scaling.
- **Single population site:** `ContextManager::EmitTrainingContext()` — the `.context` emit. Add a `RiskGateContext` sub-table to the `MarketObservationBuilder` there, populated field-by-field from `m_localRiskContext`. Because `EmitTrainingContext` and `m_localRiskContext` live in the same class, this is a local, self-contained change — no shared-writer generator, no cross-table parity mechanism.
  - FlatBuffers ordering: build the `RiskGateContext` offset *before* `MarketObservationBuilder mob(builder)` starts, then `mob.add_risk_gate_context(rgcOffset)`.
- **Live HMM path** (`HMMClient::SendBinaryRequest`, the live `MarketObservation`): **leave `risk_gate_context` unset.** The HMM reads only `observation`; an unset FlatBuffer field serializes zero bytes → no hot-path bloat, no HMM impact. (Populate later only if a live consumer of the raw gate context emerges.)
- **No changes** to `EventSerializer.cpp`, `Event`, `TrainingEvent`, or the shared-writer generator.

## 6. Python co-evolution (lbrnet)

1. Regenerate FlatBuffer bindings (both repos, same `flatc`).
2. In the `.context` reader (`context_stream.py` / `iter_context_stream`), expose `MarketObservation.RiskGateContext()`; switch every gate check to read `RiskGateContext.*` (`spread_stress`, `vpin`, `pareto_tail_alpha`, …) instead of the scaled `observation[9/11/12]`. Same record the sim already iterates — no new stream.
3. **Delete the stopgaps** — `_LIQ_FRAGILITY_TAIL_THRESHOLD`, `_VPIN_TOXICITY_TAIL_THRESHOLD_*`, the PC-16 removal, and the PC-15/16/17 comment blocks — restoring the real thresholds (`0.85`, `0.80/0.40`, raw Hill-α gate).
4. The 16-D `observation` vector reverts to being *purely* the model/HMM input — no longer double-duty as a gate proxy.
5. Close Findings 19/20/21 / PC-15/16/17.

## 7. Backward compatibility & regeneration

- Additive table fields with defaults → existing `.alpha`/`.context` parse unchanged (new field reads null/absent). Python must treat absent `risk_gate_context` as "old file → keep stopgap path" until re-collection, OR gate the switch on presence. **Graceful, no forced immediate re-collection.**
- Regenerate via `bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh`, then `./build_dll.sh`. Update both repos' generated bindings in the same change.
- Re-examine `CMakeLists.txt` schema contracts (WS-03 `MindfulTraderSchemaContract`, WS-07 shared-root-write audit) — the new shared writer must pass the parity audit.
- Doc-sync contract: the four mirror docs if guidance changes.

## 8. Acceptance / verification

1. `regenerate_schema.sh` + `./build_dll.sh` green; both-repo bindings regenerated.
2. Numeric parity: for a replay, `MarketObservation.risk_gate_context.spread_stress` in a fresh `.context` == the live `m_localRiskContext.spreadStress` at that event (and the raw twin != the scaled `observation[12]`).
3. Live HMM message unaffected: `HMMClient` `MarketObservation` still serializes with `risk_gate_context` unset (byte size unchanged vs. today) — spot-check the payload length.
4. Python: reads `RiskGateContext` from the `.context` `MarketObservation`; stopgaps deleted; gate firing rates recomputed on raw values (expect material change — cf. PC-18 becoming top rejector after the mitigations).
5. Findings 19/20/21 marked CLOSED; PC-15/16/17 stopgaps removed.

## 9. Open items

- **O1 — RESOLVED:** serialize the **full** `LocalRiskContext` (all 16 fields, per §4) — future-proof, single source of truth; new gate signals flow automatically once added to the struct.
- **O2 — RESOLVED:** home is `MarketObservation` (`.context`), where the sim already reads gates. `Event`/`TrainingEvent` untouched. `calibrate_context_thresholds.py` (a `.context`-only tool) is automatically covered — it reads `MarketObservation` too.
- **O3 — RESOLVED:** no shared-writer generator needed. Single table, single population site (`EmitTrainingContext`), same class as the data source. Direct `MarketObservationBuilder.add_risk_gate_context(...)`.
- **O4 — Payload size:** the field is populated **only** in the offline `.context` write (~+64 bytes/record — a file, acceptable). The live HMM `MarketObservation` leaves it unset → **zero** live hot-path cost.
- **O5 — Live/offline symmetry:** should the live HMM `MarketObservation` also carry `RiskGateContext`? Recommend **no** — no live consumer (C++ gates in-process), and the offline `.context` is generated from the same live `m_localRiskContext`, so gate-decision parity is already preserved.
