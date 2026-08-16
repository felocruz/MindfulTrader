# Spec: ECTS Prefix-Training Infrastructure (General Capability)

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session),
literature-reviewed via `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_103`/response.
**Split out from**: `docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md` — Turtle
Soup's Option B was originally specced inline; the user correctly reframed it as a general capability
mid-brainstorm ("I was viewing things from the point of view that the classifier could be used
generally, not just for Turtle Soup"), so this spec now stands on its own, mirroring how
`PredatorContext`/`PredatorFusion` is its own infrastructure spec separate from the individual
patterns that consume it.
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
(twin-first validation), `docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`
(the Predator Decision Contract this capability's outputs must satisfy once consumed by any pattern).

## Purpose

Build reusable Early Classification of Time Series (ECTS) infrastructure — offline prefix-dataset
construction, a stopping-rule theoretical framework, and a Predator-equipped Python twin extension —
so that any current or future pattern/decision needing genuine intra-bar anticipation (not just a
tick-reactive heuristic, but a trained, calibrated prediction of a bar's eventual shape from a partial
prefix) can consume it, rather than each one building its own one-off version.

**Two consumers already identified, neither hypothetical:**
1. **Turtle Soup's Option B** (`docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md`)
   — the first, concrete application, currently in a bridge plan alongside a cheap heuristic (Option A).
2. **TRAP's anticipatory τ* layer's own deferred need.** `CLAUDE.md`'s Trap Detection section already
   names "ECTS-style intra-bar-prefix training" as the prerequisite for genuine intra-bar model
   re-inference — today, only the τ* *threshold* recomputes every tick against a completed-bar-trained
   posterior; the model's own probability estimate does not update mid-bar, specifically because this
   capability doesn't exist yet. Building it here directly unblocks that already-identified,
   independently-valuable gap.

**Why this matters for the investment case, precisely**: whether Turtle Soup's own specific classifier
beats Turtle Soup's own specific heuristic is a genuinely open empirical question (small-sample,
single-pattern, single-instrument — a domain-expert heuristic may well generalize better; see that
spec's own honest discussion). That uncertainty applies to *that one comparison*, not to whether this
underlying capability is worth building — it has a second, real, already-scoped consumer regardless of
how Turtle Soup's own comparison turns out.

## Components

### 1. Offline Prefix-Dataset Construction (Python, general methodology)

From `mes_continuous_ticks.parquet` (already available — ~38M real per-trade ticks, microsecond
timestamps; no new live collection needed for any consumer):

- Reconstruct historical bar formation at multiple snapshot points, sampled by **elapsed-time or
  volume fraction** τ ∈ {0.1, 0.2, ..., 0.9} — **not raw tick count**, which varies systematically
  with volatility and would bias any dataset toward whichever regime produces more ticks per unit
  time (credited to the `CLAUDE_BRIEF_103` literature review).
- Label each prefix snapshot with the bar's real eventual outcome **and** whether the resulting
  decision would have cleared a minimum forward-return/utility threshold — labeling on shape alone
  risks training a model that recognizes "looks like the pattern" without recognizing "was actually
  worth acting on," a real, non-obvious failure mode distinct from ordinary overfitting (also credited
  to that review).
- This construction methodology is consumer-agnostic — the specific features extracted per prefix
  snapshot differ per consumer (Turtle Soup's penetration-vs-20-bar-extreme vs. TRAP's own structural
  distance measures), but the sampling/labeling discipline above is shared.

### 2. Stopping-Rule Framework (theoretical, shared apparatus)

- **Dachraoui et al. (2015)** cost-based sequential decision framework as the foundation for the
  earliness/accuracy tradeoff.
- **Elkan (2001)** cost-sensitive threshold (τ* = C_FP/(C_FP+C_FN)) — the same formula already used
  for TRAP's own threshold, conditioned per-consumer on regime state. Reused, not reinvented, per
  consumer.
- **Moustakides (1986)/Shiryaev** sequential change-point detection — the "penetrate-then-reject"
  or "drift reversal" class of pattern is formally a change-point detection problem; this is the same
  theoretical family `CLAUDE.md` already cites for TRAP's Shiryaev-Wald framing, not a new,
  unconnected theory per consumer.

### 3. Predator-Equipped Python Twin Extension

Bring the Predator Decision Contract's macro-input/micro-input/fusion-rule shape to the Python twin
itself — a general mechanism (mirroring the C++ `PredatorContext`/`PredatorFusion` design, not a
parallel invention), so that *any* consumer's candidate micro-signal (heuristic or ECTS-trained) can
be prototyped and empirically compared against its own baseline, entirely in Python, before any C++
commitment. This is real, shared engineering work — not a rubber-stamp step, and not duplicated per
consumer once built.

## Model & Deployment Guidance (general, applies to every consumer)

- **Classical, non-deep-learning classifiers** (scikit-learn logistic regression, or gradient-boosted
  trees only if logistic regression proves insufficient) over engineered prefix features. This
  genuinely satisfies ECTS's definitional bar (Xing, Pei & Philip 2009 — prefix discrimination +
  earliness-accuracy tradeoff, neither requiring deep learning) without deep-learning engineering
  weight. `scikit-learn` is already in `lbrnet`'s environment; no new dependency for prototyping.
- **No live Python round-trip for inference, ever, for any consumer.** Verified directly: this
  codebase has zero existing precedent for C++-side model deployment, and only `libzmq`/`libsodium`
  exist as third-party C++ dependencies today, both manually vendored under the WSL→Windows `clang-cl`
  cross-compilation setup. A per-tick ZMQ round-trip would reintroduce the exact latency this whole
  Predator initiative exists to remove — even the HMM's own live channel is Mahalanobis-gated, not
  evaluated every tick. Whichever consumer's model proves out, it deploys as **hand-crafted C++**, not
  a network call.
- **Hand-crafted C++ port, not auto-generated code, once/if a consumer's model proves out.** Logistic
  regression ports trivially (a weight vector, a dot product, a sigmoid) — essentially risk-free. A
  small gradient-boosted-trees model, if one is ever needed, gets a hand-written tree-traversal
  function reading the trained model's split-thresholds/leaf-values as plain exported data — not a
  tool like `m2cgen` generating logic, since this project's own DOD/free-function conventions and
  "understood, reviewable code" preference outweigh the marginal convenience of code generation for a
  model this small. Either way: **golden-vector regression tests proving the C++ port matches the
  Python model's own predictions are mandatory before any such port ships**, matching this project's
  existing discipline everywhere else a model or calculation gets ported between languages.
- **No new C++ ML runtime library** (ONNX Runtime, LightGBM C API, treelite) for any consumer —
  vendoring a real prebuilt library into a cross-compilation setup that has already proven non-trivial
  for just two dependencies (`libzmq`, `libsodium`) is a real integration cost this infrastructure is
  specifically designed to avoid needing.

## Test Plan

1. Dataset-construction leakage checks (no forward-looking information in any prefix snapshot) —
   a reusable test methodology, run per-consumer against that consumer's specific feature set.
2. Predator-equipped twin: unit/integration tests confirming the macro/micro/fusion shape mirrors the
   C++ `PredatorContext`/`PredatorFusion` design faithfully (this is the parity-critical piece — a
   twin that diverges from the C++ shape it's meant to validate against would defeat the entire
   point).
3. Per-consumer, once/if a trained model is proposed for C++ deployment: golden-vector regression
   tests (Python prediction vs. hand-ported C++ output) before that specific port ships — required
   regardless of which consumer.

## Non-Goals

- **No commitment that any specific consumer's classifier beats its own baseline.** This is
  infrastructure; each consumer's comparison is judged on its own empirical merits (see Turtle Soup's
  own spec for that pattern's specific, still-open comparison).
- **No deep sequence model** (LSTM/Transformer-based early-exit) for any consumer.
- **No schema changes.**
- **No live retraining pipeline** — all model training here is offline, batch, against already
  -collected tick data.
- **Not a commitment to build the TRAP anticipatory layer's consumer application now** — that
  remains its own, separately-scoped future unit (gated on Unit 3's `ExitReason_TRAP` schema work,
  itself currently deferred); this spec only ensures the shared capability it would need already
  exists once that work is picked up.

## Investigation Log

- **2026-08-16**: Originally specced inline as Turtle Soup's "Option B." The user reframed it
  mid-brainstorm as a general capability question ("I was viewing things from the point of view that
  the classifier could be used generally, not just for Turtle Soup") — correct, and it identifies a
  second real consumer (TRAP's own already-deferred ECTS need) that was there in `CLAUDE.md` the whole
  time but hadn't been connected to this work until this reframing. Split into its own spec
  accordingly, mirroring the `PredatorContext`/`PredatorFusion` infrastructure-vs-consumer pattern
  already established this session.
