# Rust Student-t HMM Lifecycle (train → posteriors → live inference) — Spec

**Status**: design only, nothing implemented, no code touched. Opened 2026-10-07 from the
monorepo-consolidation brainstorm (`2026-10-07-mindfultrader-monorepo-consolidation-spec.md` §9,
operator decision: the whole HMM lifecycle moves to Rust). Written against the real code in
`lbrnet/` and `MindfulTrader/` as read on 2026-10-07; items not verified are listed in §9.

**Standing caution.** This is a re-platforming, not a validated trading edge. The Vision section of
`PRODUCTION_TRIAGE.md` still says the HMM was trained on a contaminated vector and has never passed
its fat-tail sign-off. Porting faithfully must not be read as "the HMM is fixed". The point of doing
it now is that live, offline and training stop being three implementations.

## 1. Goal and non-goals

**Goal.** One pure Rust crate owns every HMM computation, consumed three ways: the Sierra DLL (live,
in-process, no network hop), `lbrnet` (training orchestration and offline use, via PyO3), and a
standalone tool (offline posterior files for Transformer training).

**Consequences that are the point:**

- The 5561 DEALER/ROUTER and the Python `RegimeEngine` serving path are deleted, not migrated.
- The posteriors the Transformer *trains* on and the ones C++ sends it *live* come from the same code:
  train/serve skew is impossible by construction.
- The HMM state is in-process for C++ — available to `RiskGateContext` (the 2026-09-04 audit found 7 of
  8 hard gates blind to HMM state), and `BackTesterStudy` replay no longer depends on a ZMQ server thread.

**Non-goals.** No change to the model family (diagonal Student-t, weighted emissions), to K, to the
observation vector, or to any threshold. No Transformer change beyond where it reads HMM output from.
No change to `.context` / `.alpha` formats.

## 2. What exists today (verified 2026-10-07)

**Live path (per HMM step, fired by the Mahalanobis significant-change gate in C++):**
`HMMClient::RequestUpdateAsync` → DEALER → Python ROUTER (`lbrnet/backtest/backtest_server.py:598`;
`MTS/zmq_client.py:323` binds a second one) → `FeatureSpine` pairs `MARKET_OBSERVATION` with
`SYSTEM_STATE` by sequence id → burn-in → `LiveAgent.predict_hmm` → `RegimeEngine.infer` → a
`RiskStateUpdate` back to `HMMClient::HandleBinaryResponse`, which validates, normalizes the
probabilities and writes `HmmStateIndicator::SetState` / `ClimateIndicator::SetClimate`, and applies
the piggybacked `reinfer_action_id` to `PredictionState`.

**What `RegimeEngine.infer` computes** (`lbrnet/lbrnet/models/regime_engine.py:792`), all to be ported:

| Output | Source |
|---|---|
| Model input | raw 18D observation, projected through `HMM_KEEP_DIMS` (`slice_for_gmm`, `hmm_utils.py:433`; `fast_mean_rev_z` is excluded), cast to float32 |
| Posterior | Hamilton (1989) forward filter in log space (`_hamilton_belief_update`, line 1104): `log_pred = logsumexp_j(logA[j,k] + log_belief[j])`, add emission, renormalize. First step starts from `startprob_`. The belief persists across calls. `batch_causal_filter` (offline) calls the same function. |
| Emission | weighted Student-t, `StudentTHMM._log_emission_and_delta` (`student_t_hmm.py:586`): `delta = Σ w_d (x_d−μ_d)²/σ²_d`; `log b = lnΓ((ν+D)/2) − lnΓ(ν/2) − ½(D ln(νπ) + Σ ln σ²) + ½ Σ ln w_d − ½(ν+D) ln(1+delta/ν)`, with D the **true** dimension (not Σw) and ν clipped to `[dof_min, dof_max]` |
| Tail diagnostics | `compute_tail_diagnostics`: `dof`, `mahalanobis = √delta`, `tail_weight = (ν+D)/(ν+delta)` |
| Derived | argmax state; normalized entropy; `confidence = p_max·(0.6 + 0.4·(1−H))`; `transition_risk = 1 − A[s,s]`; `expected_duration = 1/max(transition_risk,1e-6)`; `σ_eff = p·σ_per_state` |
| Climate | manifest profile lookup + overrides: Taleb fat-tail (`0<tail_alpha<1.5 ∧ fragility>3` → `TALEBIAN_FRAGILE`), Pareto mirror (`GAUSSIAN_STABLE ∧ tail_alpha>2 ∧ fragility<1 ∧ burstiness>2` → `PARETO_MOMENTUM`), burstiness EMA with enter/exit deadzone hysteresis, entropy-chaos override → `SHANNON_CHAOS`; "unconditional" states exempt |
| Model-risk stateful | variance ratio (rolling-window median of per-dim realized/model variance, drift counter that decrements), black-swan streak on `tail_weight`, Amari FIM nearest-rival distance, duration tracking, entropy-drift alerts |

Not part of the contract and **not to be ported as-is**: `_project_probs_to_hmm_enums` (its own docstring,
2026-09-22, says its premise is broken); `cognitive_load`/latency (wall-clock dependent, so
non-deterministic — decide in §9 whether it survives at all).

**Offline path.** `lbrnet/lbrnet/scripts/materialize_hmm_features.py::compute_context_posteriors`
writes `<name>.context.posteriors.npy` (a structured array) plus a `.meta.json` cache key. Measured:
the 2026-09-20 file is **21,967,249 rows**, record = `ts_us i8, bar_index i4, dominant_regime i1,
p_coiled/p_gaussian_stable/p_gaussian_fragile/p_pareto f4, mahal_distance f4, obs_vec f4[18]` (so four
hard-coded named probabilities, K=4), 2.3 GB; the `.alpha` it joins to is ~26 MB. The meta cache key is
`context_size`, `context_mtime`, `hmm_model_path/size/mtime`, `bar_duration_us`.

**Training.** `lbrnet/lbrnet/scripts/train_student_t_hmm.py` (>4,500 lines, dozens of CLI options incl.
a grid search over K=4..7) over `models/hmm_model.pkl` (6.6 KB) and `models/ModelManifest.json`
(`state_map` ×4, `gang_validation`, `metadata`). The model (`student_t_hmm.py`, 1,693 lines + a 501-line
GPU variant): diag covariance only, ECME degrees-of-freedom update (`brentq` root-find), MAP-EM priors,
sticky diagonal prior, per-feature weights, crash-state init override.

**Atratus's HMM as a starting point** (`Atratus/sensor_core/src/hmm.rs`, `rust/src/trainer.rs`):
diag Student-t, `ndarray`, `rayon` EM, k-means++ init, sticky-HDP prior, a saliency analysis,
manifest + `RegimeRegistry`, versioned model file with SHA-256 checked by `deploy_model.sh`. **Gaps vs
MTS:** no per-feature weights in the emission, probability-space (not log-space) filter, no ECME dof
step, none of the §2 model-risk diagnostics, `forward_step` allocates. It is a base to extend, not a drop-in.

## 3. Target architecture

```
core/                              Cargo workspace in the monorepo (lives beside cpp/, per consolidation spec §8)
├── hmm/            pure crate: no zmq, pyo3, ibapi, polars. Model, emission, filter, tail diagnostics,
│                   regime engine (classification + model-risk state), EM trainer, artifact I/O.
├── hmm_ffi/        staticlib + cbindgen header, consumed by cpp/ (hmm_init / hmm_step / hmm_reset)
├── hmm_py/         PyO3 module for lbrnet (train, filter, batch_causal_filter, load/save artifact)
└── tools/hmm_tool  CLI: train, posteriors, verify-golden, export
```

**Artifact (replaces `models/hmm_model.pkl`).** A pickle cannot be read by Rust and is a poor
deployment format. Proposal: the existing `ModelManifest.json` (extended with dims, `HMM_KEEP_DIMS`,
feature weights, dof bounds, thresholds the regime engine hard-codes today) plus a flat
little-endian float64 weights file with a small header and a SHA-256 in the manifest. Deliberately not
`bincode` (Rust-only), so Python tools can still read it with `numpy.frombuffer`. All dimension
indices the regime engine reads from the observation (`_TAIL_INDEX_IDX`, `_LIQ_FRAGILITY_IDX`,
`_BURSTINESS_IDX`) come from the schema-generated dimension constants, never hard-coded — this repo has
already been bitten twice by misaligned per-dim arrays (`FeatureScaler`).

**Live (DLL).** `hmm_step(obs18_f32, ctx) -> HmmStep` called synchronously from the existing HMM-trigger
site, ~µs for K=4, D≈17. Allocation-free: all buffers (belief, variance-ratio window, duration history)
sized once at `hmm_init` from the manifest (K is runtime), never per step. Everything
`HandleBinaryResponse` does today (range checks on state/climate, probability validation and the
reject/clip counters, setters) moves to a thin wrapper around the C-ABI result; the staleness gate
(`IsHmmStateStale`, 5 s) loses its reason to exist. Every `extern "C"` function is `catch_unwind`-guarded
(a panic inside Sierra is a crash).

**Offline.** `hmm_tool posteriors` streams `.context` (Rust reader over the `flatc --rust` bindings,
consolidation spec P3; hard-refuse on `schema_version` mismatch exactly as `context_reader.h` does) through
the filter and writes the **existing** sidecar byte layout so Transformer training code is unchanged in
the first version. The npy structured-dtype header is hand-written (`ndarray-npy` does not do structured
dtypes). The meta cache key switches from path/size/mtime to a content hash of model + context.

**Transformer coupling (inverted, one-way).** C++ publishes the HMM output with each event on the
existing event stream: additive schema fields (non-breaking, same pattern as
`docs/ADR/risk_gate_context_wire_spec.md`), reusing the `HmmRiskState` fields where possible. The Python
Transformer reads regime features from the event instead of running its own `RegimeEngine`. The
piggybacked `reinfer_action_id` on `RiskStateUpdate` is removed; Python detects the state flip in the
event stream and re-infers on its own. **Coherence ruling (operator, 2026-10-07: "appropriate").** The piggyback existed to "eliminate the
coherence gap between HMM and Transformer updates"; with the HMM in C++ the HMM state is current
immediately and a Transformer action arrives later. Rule:

1. The C++ HMM state is authoritative. Every acknowledged HMM step is stamped with its `sequence_id`.
2. A Transformer action carries the `sequence_id` of the event it was inferred from. C++ already stores
   this (`PredictionState::SequenceId()`, `include/Indicator.h`), and already discounts by event-count
   ageing (`SemanticFreshnessDiscount`: 0 events → 1.00 … 5+ → 0.00). That mechanism is unchanged.
3. C++ pairs the action with the HMM state of the same `sequence_id`. An action whose `sequence_id`
   precedes the sequence at which the current acknowledged HMM state began (the last regime flip) was
   inferred under a different regime and is **stale**: it is not acted on and its freshness is 0.
4. The native floor is unaffected: stale or missing Transformer output never suppresses native logic
   (consistent with the trap-detection arbitration rule: the model may lead, never suppress).

Interpretation to confirm (§9-1): "older-sequence" is applied against the **regime epoch** (the
sequence of the last HMM flip), not against every newer sequence, because HMM steps fire continuously
and generic ageing is already handled by `SemanticFreshnessDiscount`. After a flip, Python detects it in
the event stream and re-infers; that action carries a sequence at or after the flip and is accepted.

## 4. Parity strategy

**Inference must match Python numerically; training cannot, so it gets a statistical gate.**

- *Inference:* Rust reads the same float32 observation and widens to float64 where numpy does (the
  Python cast is `float32` observation, `float64` model); the filter is log-space like Python's.
  Tolerance: probabilities ≤ 1e-9 (f64), tail diagnostics ≤ 1e-9, stateful fields (EMA, streak and
  duration counters, drift count) **exact**, every output field checked, not a sample.
- *Training:* EM is non-convex; bit-exactness is not a sensible claim. Gate: same `.context`, same seed
  and init, compare held-out log-likelihood and BIC, state means/covariances/dof after state alignment
  (the repo already has a state-alignment spec: `HMM_STATE_ALIGNMENT_SPEC.md`), and posterior agreement
  on a fixed observation set. State the tolerances before running, not after.
- *Order matters:* change one thing at a time. Prove inference parity on the **existing frozen model**
  before Rust ever trains a model, so a discrepancy cannot be blamed on training.

## 5. Stages and gates

**A — Golden harness (Python only, no Rust).** (1) `export_hmm_artifact.py`: pickle → the §3 artifact.
Gate: reloading the exported arrays in Python reproduces `RegimeEngine` outputs exactly (proves the
export is lossless). (2) `make_hmm_golden.py`: run the real `RegimeEngine` sequentially over a real
`.context` slice (1M+ rows spanning regime changes, incl. the burn-in head) and record observation +
**every** output field per step into a golden file. Gate: golden regenerates deterministically
(run twice, identical).

**B — Rust inference core.** Emission, log-space filter, tail diagnostics, artifact loader. Gate:
Stage A golden, probabilities and tail diagnostics within §4 tolerance. Compare against
`Atratus/sensor_core/src/hmm.rs` only for reuse, not as a reference.

**C — Regime engine port.** Classification + overrides, deadzone hysteresis, variance ratio, black-swan
streak, FIM distance, duration tracking, entropy normalization. Gate: all golden fields; stateful fields exact.

**D — Offline posteriors tool.** Rust reader of `.context`, filter, existing-layout npy writer. Gate:
vs the Python-written 21.97M-row file: probabilities ≤ 1e-6 (they are float32), `dominant_regime` equal
except ties (list them), meta key valid; then a Transformer training dry run on both sidecars with
matching early loss.

**E — In-process live, shadow mode.** `hmm_ffi` linked into the DLL (toolchain per consolidation spec
§8: clang-cl + xwin sysroot). Both paths run in a full `BackTesterStudy` replay: the 5561 reply and the
in-process step; log every divergence. Gate: zero divergence beyond §4 tolerance for the whole replay,
and replay output (`.btst`) identical to the pre-change run. Only then make the in-process result
authoritative.

**F — Cut the Transformer over.** Additive schema fields; C++ publishes HMM output with each event;
Python reads it; the §3 coherence rule decided and documented. Gate: Transformer predictions on a
replay identical to Stage E's with the same HMM values, and a replay case that exercises the §3 rule
(a flip makes the in-flight action stale; the post-flip re-inference is accepted; the native floor acts
throughout).

**G — Rust training.** ECME dof update (root-finder replacing `brentq`), MAP-EM priors, sticky prior,
feature weights, init (incl. crash-state override), the saliency fit if wanted. `hmm_py` exposes it so the
existing orchestration (`train_student_t_hmm.py` grid search, meta-HMM control plane, HPO) calls Rust.
Gate: §4 statistical gate; scale check on the real 22M-row set on CPU (rayon) against the current GPU
time — the work is K·D·N per E-step, so I expect it to be fine, but that is unmeasured.

**H — Delete.** After gates E–G are green: `student_t_hmm.py`, `student_t_hmm_gpu.py`, `regime_engine.py`'s
HMM parts, the 5561 server in `backtest_server.py` and `zmq_client.py`, `HMMClient.cpp`/`.h`, `FeatureSpine`
HMM pairing, the pickle artifact. Not behind a flag: the system is not in production, so deletion is the
default once nothing uses them (full-repo usage search first, per the Code Safety Rules). Update the
canonical-artifact convention (`models/hmm_model.pkl`) in all four mirror docs in the same change
(Documentation Sync Contract).

Rollback: until Stage H nothing is deleted; Stages A–F are reversible by switching the authoritative
source back to the 5561 path.

## 6. Dependencies on other work

- Consolidation spec pre-work **P2/P3** (one FlatBuffers version, `flatc --rust`) — needed by the Rust
  `.context` reader and the event-schema fields. **P1** (root-relative paths) eases tool invocation.
- The monorepo merge puts `core/` beside `cpp/` and `lbrnet/`; Stages A–D can start before it
  (Stage A is Python-only), but Stage E needs the Rust DLL link path.
- The Rust transport (consolidation §8) is independent; this work removes the port that was hardest for it.

## 7. Risks

- **Faithful port of a model that may still change** (K, features, Feature Saliency). Mitigation:
  everything model-specific in the artifact/manifest; nothing hard-coded in the crate.
- **Tolerance disputes.** Mitigation: tolerances fixed in §4 before Stage B; full-field golden, not samples.
- **Sierra-process crash from a panic or an allocation on the hot path.** `catch_unwind`; preallocated
  buffers; an allocation-tripwire test around `hmm_step`.
- **Behavior hidden in Python that is not in §2** (the live path also touches `LiveAgent` state around
  `predict_hmm`: re-inference bookkeeping, `_pending_reinference`). Stage A's golden records the
  piggybacked fields too so nothing is lost silently.
- **Schema change on the event stream** is cross-project; additive-with-defaults keeps old
  `.alpha`/`.context` parsing, as the risk-gate-context spec established.
- **Rust becomes a build prerequisite for `cpp/`.** Add to CI and the Done Checklist.

## 8. Not decided here

Whether the climate/threshold constants now hard-coded in `regime_engine.py` (`ENTROPY_CHAOS_THRESHOLD`,
`VARIANCE_RATIO_*`, `TAIL_RISK_*`, deadzone epsilons) move into the manifest (recommended, since a
retrained model should carry its own) or stay as crate constants.

## 9. Open questions / not verified

1. **Transformer coherence rule** — ruled 2026-10-07 (§3). Still to confirm: that "older-sequence" means
   older than the regime-epoch start, not older than the latest HMM step.
2. `cognitive_load`: deterministic replacement, or drop (wall-clock-derived today).
3. Entropy normalization function (`_calculate_normalized_entropy`, line 1265) and the unconditional-state
   set were not read in full; Stage A's golden will pin them.
4. `HMM_MODEL_INPUT_DIM` value (18 minus excluded dims) and `HMM_BURN_IN` — read from code in Stage A.
5. Whether `meta_hmm_control_plane.py` / `smart_tuner.py` / HPO call `train_student_t_hmm.py` as a
   subprocess or import the model — decides how thin the Python wrapper can be.
6. `train_student_t_hmm.py`'s `--expected-schema-version` default is **230** while the current schema is
   **240** (noted, not investigated): the existing training gate may already be stale.
7. The four-name probability sidecar (`p_coiled`, `p_gaussian_stable`, `p_gaussian_fragile`, `p_pareto`)
   bakes in K=4; keep for compatibility in Stage D, generalize later.
8. CPU vs GPU training time at 22M rows (Stage G).
