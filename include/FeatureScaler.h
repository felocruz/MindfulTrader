// FeatureScaler.h — pure, header-only hybrid Soft-Log-Z/Log-Z scaler for
// ContextManager's 16D observation vector. Extracted from ContextManager.h
// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md)
// so it can be natively unit-tested (tests/cpp/test_feature_scaler.cpp) —
// it has zero Sierra Chart/ACSIL dependency and was only ever unreachable by
// a standalone test because ContextManager.h itself pulls in sierrachart.h.
// Same rationale/precedent as InformationEngine.h/TailRiskEngine.h/
// StructureEngine.h already being extracted this way.

#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "generated/mts_schema_contract_generated.h"
#include "RingBuffer.h"
#include "Logger.h"

/// Hybrid Feature Scaler: Soft-Log-Z state channels + winsorized Log-Z energy channels.
///
/// Institutional rationale (Topology Preservation for Student-t HMM):
/// - State/geometry channels keep relative outlier distance via symmetric Soft-Log-Z.
/// - Energy/magnitude channels keep amplitude via rolling Log-Z with 6-sigma winsorization.
///
/// Architecture:
///   LOGZ dims:      2, 4, 12
///   SOFTLOGZ dims:  0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15
///   Warmup:         500 samples for stable rolling windows
///
/// After warmup, output is model-ready hybrid space:
/// - energy dims are winsorized z-space
/// - state dims are sign(z) * log1p(|z|)
/// No Python-side scaling needed: load .context and hit model.train().
struct FeatureScaler {
    enum class ScaleMode : uint8_t { SOFTLOGZ = 0, LOGZ = 1 };

    static constexpr size_t N_DIMS = MTS::Schema::Contract::kObservationDim;
    static constexpr float ABSOLUTE_FLOOR = 1e-8f;              ///< Numerical safety floor (per-dim minimum)
    static constexpr float RELATIVE_FLOOR_FRACTION = 0.01f;     ///< 1% of burn-in median stddev per dim

    /// Dim 11 (amihud_illiquidity): ABSOLUTE_FLOOR (1e-8) permanently zero-trapped
    /// this dimension. Amihud = |log-return| / dollar-volume; on a liquid index
    /// future its rolling MAD is ~1e-11 to 1e-10 -- two to three orders of
    /// magnitude below the floor -- so madScale < minScaleFloor fired on every
    /// single observation and the carry-forward death spiral (never leaving
    /// hasValidScaled=false) returned 0.0f forever. Unlike DIM_LZ_INDEX/
    /// DIM_RECURRENCE_INDEX/DIM_FRACTAL_INDEX below, Amihud's raw scale is NOT
    /// instrument-invariant (it scales with an instrument's price level and
    /// dollar volume, unlike those three bounded structural indices), so a
    /// static center/scale would silently break if this study ever ran against
    /// a different instrument -- lowering the floor instead preserves the
    /// existing adaptive rolling median/MAD calibration for this dim, just lets
    /// it actually clear the gate. See logs/rc_gemini.log CLAUDE_BRIEF_086/087
    /// and GEMINI_BRIEF_087_RESPONSE for the full derivation.
    static constexpr size_t DIM_AMIHUD_INDEX = 11;               ///< == OBS_AMIHUD_ILLIQUIDITY
    static constexpr float AMIHUD_ABSOLUTE_FLOOR = 1e-16f;       ///< Negligible vs. ABSOLUTE_FLOOR; still divide-by-zero-safe

    /// Dim 3 (correction_action): the binary floor-gate + carry-forward-decay
    /// mechanism used by the generic adaptive SOFTLOGZ path is the wrong tool
    /// for some dims. Empirically (tick-level replica against real MES data,
    /// matched to live telemetry within ~1pp -- see logs/rc_gemini.log
    /// CLAUDE_BRIEF_097/098) dim3 sustains multi-recalibration-interval
    /// stretches of tight raw-value clustering (its formula is RV_recent/
    /// RV_full, and the recent window is a strict subset of the full one,
    /// making quiet-then-break transitions more common than for dim1's
    /// disjoint-halves formula). When that happens the live madScale sits
    /// just ABOVE minScaleFloor (so the floor-gate's carry-forward branch
    /// never engages) but is still tiny, so the raw, completely unclamped
    /// z = (current-median)/madScale computed by the generic path can reach
    /// z ~ 10^4-10^5 before the outer ToSoftLogZ winsorization silently
    /// clamps it -- i.e. the failure is invisible from the public API; only
    /// the elevated rail-hit rate shows. Root cause is a scale-RELIABILITY
    /// problem, not a scale-MAGNITUDE problem, so instead of a binary
    /// trust/no-trust gate, an affected dim blends the live local MAD with a
    /// slow, continuously-updated long-horizon EWMA scale (Ledoit-Wolf-style
    /// shrinkage target).
    ///
    /// GENERALIZED 2026-08-14 (was dim3-only via a single
    /// DIM_MACRO_SHRINKAGE_INDEX constant): the same real-data trace method
    /// D4 used for dim3 (top-|z| events' local MAD sitting at/below the
    /// series' own p1, i.e. a modest raw deviation landing on a collapsed
    /// scale) independently reproduced the identical signature for dim9
    /// (tail_index, z=1963 from raw moving only 1.4->8.0 while local MAD sat
    /// at 0.00336, far below its own p1=0.0249), dim0 (log_variance_ratio,
    /// z=-176 from raw moving only ~0.25 while local MAD sat at 0.00144,
    /// below its own p1), and dim7 (micro_asymmetry, z=438 with local MAD
    /// consistently below its own p1 across all top-10 events) -- three more
    /// dims with the exact same underlying weakness, not three unrelated
    /// per-dim quirks. Shrinkage-toward-a-stable-anchor (Ledoit-Wolf) and a
    /// variance floor (GARCH omega) are general small-sample scale-estimation
    /// principles, not properties of dim3's specific formula -- treating this
    /// as a dim3-only special case was an under-generalization now corrected.
    /// Per-dim opt-in via a nonzero SHRINKAGE_SCALE_MIN entry (0.0f = this
    /// dim keeps the simpler generic floor-gate/carry-forward path below --
    /// not every dim needs this; dims with no measured collapse signature
    /// should NOT be forced through the extra complexity). See
    /// docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md
    /// D4 (original dim3 derivation) and its generalization addendum.
    ///
    /// Shrinkage-weight signal: NOT dominanceRatio[]. First implementation
    /// tried reusing the existing D2 dominanceRatio diagnostic (exact-value
    /// repetition) as the reliability signal, on the theory that a collapsed
    /// window would also be a repetition-dominated one -- checked against the
    /// real fixture and it wasn't: dominanceRatio stayed in 0.07-0.22 for the
    /// entire ~35k-sample approach to the known pathological event (never
    /// near enough to a 0.30 sigmoid midpoint to shrink meaningfully), while
    /// madScale had independently collapsed to 0.22% of macroScaleEwma at
    /// that exact point. dominanceRatio measures ONE EXACT VALUE repeating;
    /// this failure mode is many DISTINCT values compressed into a narrow
    /// band, which a repetition counter cannot see. The signal that actually
    /// tracks this is the ratio between the live local MAD and the long-
    /// horizon macro reference itself -- computed inline in UpdateAndNormalize
    /// below. See GEMINI_BRIEF_097_RESPONSE/098_RESPONSE for the literature
    /// grounding (RiskMetrics EWMA, GARCH variance-floor practice, MAD's
    /// documented discontinuous-influence-function instability under regime
    /// persistence, Ledoit-Wolf shrinkage as the blending framework).
    ///
    /// EWMA decay per dedupe-survived push toward a long-horizon reference
    /// scale. 0.99995 -> half-life = ln(2)/-ln(0.99995) ~= 13,863 samples,
    /// long enough to survive one 5,000-sample recalibration interval intact
    /// (engineering choice -- no literature-prescribed exact figure; matches
    /// this file's existing convention for constants like kAlphaSmoothing in
    /// TailRiskEngine.h). Shared across every shrinkage-enabled dim -- no
    /// evidence yet that per-dim tuning of decay/midpoint/steepness is
    /// needed; only the scale floor itself is dim-specific (each dim's raw
    /// value lives on a completely different magnitude).
    static constexpr float MACRO_SCALE_DECAY = 0.99995f;
    /// Per-dim GARCH-omega-style irreducible minimum for macroScaleEwma -- a
    /// purely reactive EWMA-of-observed-deviations anchor cannot protect
    /// against the FIRST large deviation after a genuinely quiet regime,
    /// because it can only learn a "normal" scale from deviations it has
    /// already observed (chicken-and-egg). 0.0f = shrinkage disabled for
    /// this dim (use the generic floor-gate/carry-forward path instead);
    /// nonzero enables shrinkage with this dim's own empirically-derived
    /// floor -- NOT interchangeable across dims, each was derived from that
    /// dim's own real rolling-local-MAD distribution (5th percentile,
    /// sampled every 500 observations across the real 2023-09-01..10-20
    /// tick-replay window, same D4 methodology, giving real margin above the
    /// observed pathological minimum without interfering with normal
    /// regimes):
    ///   dim3 (0.00007): original D4 derivation, window=300.
    ///   dim9 (0.0438):  tail_index, window=500. p1=0.0194 p5=0.0438(chosen)
    ///                   p25=0.1907 p50=0.4372 -- Hill alpha lives on a
    ///                   ~1.1-8.0 scale, orders of magnitude above dim3's.
    ///   dim0 (0.000389): log_variance_ratio, window=500. p1=0.000165
    ///                    p5=0.000389(chosen) p25=0.001741 p50=0.003405.
    ///   dim7 (0.00733): micro_asymmetry, window=300. p1=0.004214
    ///                   p5=0.00733(chosen) p25=0.018797 p50=0.041481 --
    ///                   bounded [-1,1] by construction (ofae::
    ///                   ComputeMicroAsymmetry), still shows the same
    ///                   collapse-then-modest-deviation signature.
    ///   dim4 (0.211521): vol_convexity, window=500. p1=0.177302
    ///                    p5=0.211521(chosen) p25=0.250188 p50=0.282043.
    ///                    First LOGZ dim to use this mechanism (originally
    ///                    SOFTLOGZ-only) -- found via the same top-|z|-event
    ///                    trace, then confirmed at full population scale
    ///                    (not just the top 5): of 85 real exceedance events,
    ///                    51 (60%) showed the collapsed-local-MAD signature,
    ///                    34 (40%) did not -- a genuinely mixed population,
    ///                    unlike every other traced dim's clean all-or-
    ///                    nothing result. Shrinkage handles the contaminated
    ///                    majority without needing to first separate the two
    ///                    populations by hand; ComputeShrinkageZ() (shared
    ///                    helper, extracted from the SOFTLOGZ-only version
    ///                    that existed before this) is now used by both
    ///                    scaling paths.
    inline static std::array<float, N_DIMS> SHRINKAGE_SCALE_MIN = {  // compiled defaults; overwritten in place by LoadConfig()
        0.000389f,  //  0  log_variance_ratio   confirmed collapse signature (z=-176 traced)
        0.0f,       //  1  burstiness_index     disabled -- tail decays cleanly under generic path + wide bound
        0.0f,       //  2  relative_range       disabled -- LOGZ, 0.035% clip rate, no evidence of need
        0.00007f,   //  3  correction_action    original D4 derivation
        0.211521f,  //  4  vol_convexity        LOGZ dim, confirmed collapse signature (see LOGZ generalization note below)
        0.0f,       //  5  lempel_ziv           disabled -- static scaler, not applicable
        0.000145f,  //  6  hurst_exponent       confirmed collapse signature, exact-formula-derived (see below)
        0.00733f,   //  7  micro_asymmetry      confirmed collapse signature (z=438 traced)
        0.0f,       //  8  fisher_info          audited 2026-08-14 -- clean, no collapse signature; needs only a wider bound (DIM_WINSOR_SIGMA_OVERRIDE), not shrinkage
        0.0438f,    //  9  tail_index           confirmed collapse signature (z=1963 traced)
        0.0f,       // 10  skewness_idx         audited 2026-08-14 -- 0.041% clip rate, only 31 exceedances (too few for a reliable fit); weak collapse signal present but impact is negligible, disabled per ruthless-simplicity (D8's own precedent: don't force complexity where the flat default already performs fine)
        0.0f,       // 11  amihud_illiquidity   audited 2026-08-14 -- 0.000% real production clip rate (CLAUDE_BRIEF_095), bar-gated/historical-only construction (never sees the live still-forming bar, unlike the dims above), already has its own dedicated floor fix (AMIHUD_ABSOLUTE_FLOOR) -- no action needed
        0.0f,       // 12  liq_fragility        disabled -- LOGZ, resolved-healthy, no evidence of need
        0.0f,       // 13  recurrence_rate      disabled -- static scaler, not applicable
        0.0f,       // 14  fractal_dim          disabled -- static scaler, not applicable
        0.0f,       // 15  mean_rev_z           audited 2026-08-14 -- 0.028% clip rate, only 14 exceedances (negligible); no action needed
    };
    /// dim6's floor (0.000145) was RE-DERIVED 2026-08-14 (same-day follow-up)
    /// against an EXACT port of the production DFA algorithm
    /// (`CalculateHurstExponent`, `StudyHelperFunctions.cpp:2489`) -- the
    /// first attempt used a Python re-implementation approximate in spirit,
    /// not an exact port (different scale-selection sampling), and was
    /// correctly flagged as lower-confidence pending this re-derivation.
    /// The exact replica ported every detail: the `step=(maxScale-minScale>50)
    /// ?2:1` scale-sampling rule, the closed-form per-segment least-squares
    /// identical to the C++ (not `np.polyfit`), `length=100`
    /// (representative `macro_window_n`), `minScale=8` (the literal call-site
    /// constant). Result was surprising enough to warrant a dedicated
    /// investigation (`logs/rc_gemini.log` `CLAUDE_BRIEF_101`/`102`,
    /// `GEMINI_BRIEF_101`/`103_RESPONSE`): pre-shrinkage `|z|>=6` rate was
    /// `24.851%` with `max|z|=3085.93` -- an order of magnitude worse than
    /// any other dim's pre-fix state. Two competing hypotheses: genuine tail
    /// signal, or a DFA-instability artifact of re-estimating a slow,
    /// macro-timescale statistic every tick. Settled empirically via a
    /// tail-conditional noise decomposition (compare intra-bar tick-to-close
    /// Hurst movement during collapsed-local-MAD periods vs normal periods):
    /// noise was **4.3x SMALLER** during the exact episodes producing the
    /// extreme z-scores (`std=0.0092` vs `0.0394`), the opposite of what an
    /// instability artifact would show -- ruling out the DFA-noise hypothesis
    /// and confirming this is the same scale-collapse-then-modest-deviation
    /// mechanism as dim3/dim9/dim0/dim7/dim4, just far more prevalent for
    /// this dim (quiet Hurst clusters are common, real regime shifts from
    /// them are frequent). Post-shrinkage: `|z|>=6` rate `14.803%`,
    /// `max|z|=160.11` (shrinkage alone cut the worst events 19x), a real,
    /// large (`n_tail=11,023`) GPD-characterizable tail, `shape(xi)=+0.3014`
    /// (Frechet/unbounded) -- see `DIM_WINSOR_SIGMA_OVERRIDE[6]`'s comment
    /// for the resulting bound.
    /// Compression-ratio sigmoid center: madScale/macroScaleEwma == 0.30 is
    /// where "trust local less" begins -- i.e. the local window's dispersion
    /// has visibly compressed to under a third of its own long-run typical
    /// value (engineering choice, tunable; chosen to fire well before the
    /// pathological case observed at ratio ~0.0022, with margin).
    static constexpr float SHRINKAGE_RATIO_MIDPOINT = 0.30f;
    /// Sigmoid steepness in log-ratio space: at ratio == midpoint * e^(+-0.15)
    /// (roughly midpoint scaled by 1.16x / 0.86x), w moves ~0.73 <-> ~0.27
    /// (engineering choice, tunable; solved from w=1/(1+exp(-k*0.15))=0.73).
    static constexpr float SHRINKAGE_STEEPNESS = 6.63f;

    static constexpr float LOG_BPS = 10000.0f;                  ///< Basis-point multiplier inside log transform
    static constexpr float LOG_EPS = 1e-8f;                     ///< Non-zero floor before log transform
    static constexpr float ENERGY_WINSOR_SIGMA = 6.0f;          ///< 6-sigma winsorization for energy channels (default; see LOGZ_WINSOR_SIGMA_OVERRIDE)
    static inline float STATE_WINSOR_SIGMA = 6.0f;              ///< 6-sigma winsorization before Soft-Log map (default; see DIM_WIDE_WINSOR_INDEX). Compiled default; overwritten in place by LoadConfig() if config/execution_params.json is present.

    /// Per-dim override for the LOGZ path's hard z-clamp (dims 2, 4, 12 --
    /// EXPECTED_LOGZ_DIMS), added 2026-08-14 (Task 4 of the full-coverage
    /// audit) -- the LOGZ path previously had NO per-dim mechanism at all,
    /// `std::clamp(zLog, -ENERGY_WINSOR_SIGMA, ENERGY_WINSOR_SIGMA)` was
    /// unconditional, same gap SOFTLOGZ's `DIM_WINSOR_SIGMA_OVERRIDE` closed
    /// for that path. Same 0.0f-sentinel convention (no override = use the
    /// flat 6.0 default).
    ///   dim2 (relative_range): audited, full-history bar-close screen
    ///     (75,085 real 15-min-aggregated 60min bars -> 18,761 valid
    ///     windows) found ZERO exceedances, matching production's own
    ///     already-tiny 0.035% clip rate -- closed clean without a
    ///     dedicated tick-level replica (the cheap check already answered
    ///     the question convincingly, same principle Task 3 used for
    ///     dim11/dim15).
    ///   dim4 (vol_convexity): confirmed bar-gated/historical-only by source
    ///     comment (`StudyHelperFunctions.cpp`, `CalculateVolConvexity`) --
    ///     no live-bar undersampling risk. First-pass full-history
    ///     (75,059-sample) replica found a real but small tail (85
    ///     exceedances, 0.11%) with what looked like an ambiguous
    ///     scale-collapse signature in a top-5 sample. Classifying the FULL
    ///     85-event population (not just the top 5) resolved it decisively:
    ///     51 (60%) showed the collapsed-local-MAD signature, 34 (40%) were
    ///     genuine -- dim4 needed SHRINKAGE_SCALE_MIN (see that array's
    ///     doc comment), the first LOGZ dim to use the mechanism (originally
    ///     SOFTLOGZ-only, generalized via the shared ComputeShrinkageZ()
    ///     helper). Corrected fit on the shrinkage-blended z: n_tail=76
    ///     (0.10%), shape(xi)=-0.3580 (Weibull/bounded), theoretical endpoint
    ///     8.837. Set to 12.0 -- real margin (~36%) given the still-modest
    ///     sample size, same proportional-margin logic as dim8's smaller-
    ///     sample case.
    ///   dim12 (liq_fragility): also bar-gated/historical-only (confirmed
    ///     via source). Full-history (75,051-sample) replica: clean trace
    ///     (top-5 events' local MAD all comfortably above the series' p1),
    ///     n_tail=207 (0.28%, a real sample, not a handful of points),
    ///     shape(xi)=-0.2276 (Weibull/bounded), theoretical endpoint 11.485.
    ///     Set to 12.0 -- just past the wall, same "close to the theoretical
    ///     ceiling" logic as dim1/dim9's finite-endpoint dims.
    inline static std::array<float, N_DIMS> LOGZ_WINSOR_SIGMA_OVERRIDE = {  // compiled defaults; overwritten in place by LoadConfig()
        0.0f, 0.0f,
        0.0f,   //  2  relative_range   audited, closed clean (0 exceedances on 18,761 real bars)
        0.0f,
        12.0f,  //  4  vol_convexity    GPD-derived on corrected (shrinkage-blended) z, real margin above the theoretical wall
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        12.0f,  // 12  liq_fragility    GPD-derived, clean trace, real margin above the theoretical wall
        0.0f, 0.0f, 0.0f,
    };

    /// dim1 (burstiness_index) over-saturates under the uniform 6-sigma bound
    /// (empirically: 7.63% rail-hit rate on a real tick-level replica against
    /// mes_continuous_ticks.parquet, matching live telemetry's 7.85% within
    /// 0.2pp) -- its tail tapers cleanly: full-series verification (corrected
    /// methodology, see dim3 note below) confirms max|z|=35.86 across all
    /// 1,467,159 real samples, only 0.1767% still beyond 25-sigma (matches
    /// the originally-reported 0.18% almost exactly -- this number was always
    /// sound). Widening the bound alone is the correct and sufficient fix.
    ///
    /// Set to 45.0 (not 25.0), the widest the evidence supports rather than a
    /// conservative intermediate point: a Peaks-Over-Threshold GPD fit
    /// (threshold u=6, n_tail=112,722/1,467,159=7.68%) gives shape xi=-0.1956
    /// (Weibull/bounded domain -- a genuine finite theoretical endpoint,
    /// u-scale/xi = 6-7.4076/(-0.1956) = 43.871), confirmed real rather than a
    /// single-fit artifact via a 2000-resample bootstrap (2000/2000 draws
    /// finite; endpoint distribution p5=43.25 p50=43.87 p95=44.52 max=45.12).
    /// 45.0 clears the entire bootstrap distribution. Widening this far is
    /// safe for the downstream consumer -- the 16D vector is a near-exclusive
    /// Student-t HMM input (StudentTHMM, lbrnet/models/student_t_hmm.py),
    /// whose M-step already downweights extreme observations via the
    /// Peel & McLachlan (2000) EM weight w_i=(nu+dim)/(nu+delta_i) applied to
    /// both the mean and (diagonal-only, so no cross-dim ill-conditioning)
    /// covariance update -- confirmed by reading that file directly, not
    /// assumed. See logs/rc_gemini.log CLAUDE_BRIEF_099/100 and
    /// docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md.
    /// dim3 does NOT share dim1's bound -- an earlier version of this file did
    /// (reasoning: "dim3's true tail is tame, max|z|=10.69, zero beyond
    /// 25-sigma"), which was WRONG, traced to a real bug in the diagnostic
    /// script that produced that number (constructed a std::vector<float>
    /// from a byte range, implicitly converting each individual BYTE to a
    /// float instead of reinterpreting 4-byte groups -- corrupted data, not a
    /// real measurement). Corrected full-series check (proper
    /// vector<char>+memcpy reinterpretation) shows dim3's true tail is
    /// dramatically heavier than dim1's and shaped completely differently:
    /// p50=9.50, p90=24.22 (already at dim1's entire bound), p99=271.61,
    /// p99.9=393.49, max=490.19 across the real 1,467,159-sample series --
    /// p90-to-max is a ~20x span vs. dim1's ~1.8x (19.78->35.86), i.e. a small
    /// population of genuinely very large events, not a smoothly-tapering
    /// tail. At 25-sigma, 0.1585% of ALL samples (9.77% of the 6-sigma
    /// population) would still be flattened -- confirmed via cluster tracing
    /// (see DIM_MACRO_SHRINKAGE_INDEX's Calibrate()/ratchet comments and
    /// docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md
    /// D5) that these are genuine large market moves (healthy, non-degenerate
    /// local/macro scales at the time), not estimator artifacts -- so per the
    /// same Taleb/EVT reasoning that motivated D3 in the first place, they
    /// must not be flattened either.
    ///
    /// Set to 4587.0 (not 500.0), the widest the evidence supports rather
    /// than "observed max plus margin." Unlike dim1, dim3's GPD fit gives
    /// shape xi=+0.6583 (Frechet/unbounded domain -- genuinely no finite
    /// theoretical endpoint), so "as wide as findings can make it" cannot
    /// mean a wall; it means the widest return level the actual collected
    /// evidence supports without extrapolating past it. Anchored to a target
    /// exceedance probability of p=1/N (N=1,467,159, the full historical
    /// sample count already studied) -- "as extreme as an event we'd expect
    /// to see about once across everything we've ever recorded" -- via the
    /// standard POT return-level formula b = u + (scale/xi)*((p/zeta_u)^-xi - 1)
    /// with u=6, scale=3.9710, zeta_u=n_tail/n_total=23,798/1,467,159:
    /// p=1e-5 -> 783 | p=1/N~=6.8e-7 -> 4,587 (chosen) | p=1e-7 -> 16,228 |
    /// p=1e-8 -> 73,889. Going past p=1/N stops being "what the findings
    /// support" and becomes "how far we're willing to extrapolate beyond
    /// them" -- a materially weaker justification, so 4,587 is the ceiling
    /// this derivation defends. Safe for the downstream HMM for the same
    /// reason as dim1's widening -- see that constant's doc comment.
    ///
    /// GENERALIZED 2026-08-14: was two single-dim mechanisms
    /// (DIM_WIDE_WINSOR_INDEX/WIDE_STATE_WINSOR_SIGMA for dim1;
    /// DIM3_WIDE_WINSOR_SIGMA hardcoded into the shrinkage branch for dim3),
    /// now one per-dim lookup covering both paths. 0.0f = no override, use
    /// STATE_WINSOR_SIGMA (6.0) -- the vast majority of dims have shown no
    /// evidence of needing anything wider.
    ///
    /// dim9/dim0/dim7 re-derived 2026-08-14 on the CORRECTED, shrinkage-
    /// blended z-series (SHRINKAGE_SCALE_MIN above) -- their first-pass GPD
    /// candidates (dim9 xi=0.78/max|z|=1963 -> a 360,000-sigma "bound") were
    /// fitted on the pre-shrinkage, scale-collapse-contaminated z and were
    /// artifacts, not real tail signal. The corrected fit is dramatically
    /// different in kind, not just magnitude: dim9's shape flips sign
    /// entirely, from apparent unbounded-Frechet to genuinely bounded-
    /// Weibull, same domain as dim1. Real 1,498,760-1,498,761-sample series
    /// (2023-09-01..10-20), threshold u=6, same POT/GPD methodology as D7:
    ///   dim9 (tail_index): n_tail=24,243 (1.62%), shape(xi)=-0.3259
    ///     (Weibull/bounded), scale=1.2019 -> theoretical endpoint
    ///     u-scale/xi = 6-1.2019/(-0.3259) = 9.687. Set to 10.0, just past
    ///     the wall (same "close to the theoretical ceiling" logic as
    ///     dim1's 45.0, no bootstrap ceremony needed for a ~1.7x widening
    ///     this modest -- unlike dim1's originally-surprising 45, this
    ///     result is small enough that the qualitative finding, xi<0 at
    ///     all, is the load-bearing claim, not the third decimal digit).
    ///   dim0 (log_variance_ratio): n_tail=17,584 (1.17%), shape(xi)=0.2533
    ///     (Frechet/unbounded), scale=5.9447. p=1/N (N=1,498,761) return
    ///     level = 261.66, same p=1/N convention as dim3. Set to 262.0.
    ///   dim7 (micro_asymmetry): n_tail=20,994 (1.40%), shape(xi)=0.0580
    ///     (barely Frechet, near-exponential), scale=2.2044. p=1/N return
    ///     level = 35.69. Set to 36.0.
    /// See docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md
    /// generalization addendum for the full derivation and the contaminated
    /// first-pass numbers this supersedes.
    ///
    /// dim8 (fisher_info) audited 2026-08-14, no shrinkage needed (clean
    /// scale-collapse trace): raw is exactly bounded [-2.6467,+2.6467] by
    /// construction (cfc::ComputeFisherInformation's own +-0.99 clamp,
    /// verified against source, not assumed), same "genuinely bounded"
    /// category as dim1/dim9. Tick-level replica (sampled every 20 ticks,
    /// TS1/240min), n=48,308 valid z, n_tail=330 (0.68%), shape(xi)=-1.2574
    /// (Weibull), fitted endpoint 14.216 -- close to but not past the single
    /// observed max (14.22), a smaller and less certain tail sample than
    /// dim1/dim3/dim9/dim0/dim7's (330 vs 17,584-112,722), so set with real
    /// margin (20.0, ~40% above the fitted endpoint) rather than sitting
    /// right at the wall the way dim1's bootstrap-confirmed 45.0 could.
    ///
    /// dim6 (hurst_exponent) resolved 2026-08-14 -- see SHRINKAGE_SCALE_MIN's
    /// doc comment for the full investigation (exact-formula DFA replica,
    /// tail-conditional noise decomposition ruling out a DFA-instability
    /// artifact, `logs/rc_gemini.log` CLAUDE_BRIEF_101/102,
    /// GEMINI_BRIEF_101/103_RESPONSE). Post-shrinkage: n_tail=11,023 (14.80%
    /// -- the largest confirmed-genuine tail of any dim in this initiative),
    /// shape(xi)=+0.3014 (Frechet/unbounded), scale=6.5672. p=1/N
    /// (N=74,464) return level = 344.53, same convention as dim3/dim0/dim7.
    /// Set to 345.0.
    inline static std::array<float, N_DIMS> DIM_WINSOR_SIGMA_OVERRIDE = {  // compiled defaults; overwritten in place by LoadConfig()
        262.0f,   //  0  log_variance_ratio   GPD-derived on corrected z, Frechet (xi=+0.2533), p=1/N return level
        45.0f,    //  1  burstiness_index     GPD+bootstrap derived (D7)
        0.0f,     //  2  relative_range
        4587.0f,  //  3  correction_action    GPD-derived, p=1/N return level (D7)
        0.0f,     //  4  vol_convexity
        0.0f,     //  5  lempel_ziv           static scaler, not applicable
        345.0f,   //  6  hurst_exponent       GPD-derived, p=1/N return level, genuine Frechet tail confirmed via tail-conditional noise decomposition
        36.0f,    //  7  micro_asymmetry      GPD-derived on corrected z, p=1/N return level
        20.0f,    //  8  fisher_info          GPD-derived, margin above a smaller-sample fit (see above)
        10.0f,    //  9  tail_index           GPD-derived on corrected z, Weibull (xi=-0.3259), just past the theoretical wall
        0.0f,     // 10  skewness_idx         audited, negligible clip rate, no action needed
        0.0f,     // 11  amihud_illiquidity   audited, 0.000% production clip rate, no action needed
        0.0f,     // 12  liq_fragility        LOGZ -- uses LOGZ_WINSOR_SIGMA_OVERRIDE instead, this array unused for it
        0.0f,     // 13  recurrence_rate      static scaler, not applicable
        0.0f,     // 14  fractal_dim          static scaler, not applicable
        0.0f,     // 15  mean_rev_z           audited, negligible clip rate, no action needed
    };

    enum class ConfigLoadStatus : uint8_t { DEFAULTS, LOADED_FROM_FILE, FILE_MISSING, PARSE_FAILED };
    static inline ConfigLoadStatus configLoadStatus = ConfigLoadStatus::DEFAULTS;
    static constexpr const char* kConfigPathWindows = "C:/Trading/config/execution_params.json";

    /// Loads STATE_WINSOR_SIGMA/DIM_WINSOR_SIGMA_OVERRIDE/LOGZ_WINSOR_SIGMA_OVERRIDE/
    /// SHRINKAGE_SCALE_MIN from config/execution_params.json's "featurescaler_winsorization"
    /// section, called exactly once from ContextManager's constructor (never per-tick).
    /// Fail-closed: parses into locals first and only commits on full success, so a
    /// mid-parse exception never leaves the live static arrays partially overwritten;
    /// on any failure the compiled defaults above remain in effect and the failure is
    /// loudly logged (never silent), matching this repo's established config-load convention
    /// (ExecutionParams::LoadFromFile, RiskManager.cpp's GetHMMRiskPolicy).
    static void LoadConfig(const std::string& path = kConfigPathWindows) {
        std::ifstream in(path);
        if (!in.is_open()) {
            configLoadStatus = ConfigLoadStatus::FILE_MISSING;
            Logger::getInstance().log(
                "FeatureScaler: config FILE_MISSING at " + path + " -- using compiled defaults");
            return;
        }
        try {
            const std::string payload((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            nlohmann::json j = nlohmann::json::parse(payload);

            if (!j.contains("featurescaler_winsorization") ||
                !j["featurescaler_winsorization"].is_object()) {
                configLoadStatus = ConfigLoadStatus::PARSE_FAILED;
                Logger::getInstance().log(
                    "FeatureScaler: 'featurescaler_winsorization' section missing at " + path +
                    " -- using compiled defaults");
                return;
            }
            const auto& fw = j["featurescaler_winsorization"];

            float newStateWinsorSigma = STATE_WINSOR_SIGMA;
            std::array<float, N_DIMS> newDimWinsor = DIM_WINSOR_SIGMA_OVERRIDE;
            std::array<float, N_DIMS> newLogzWinsor = LOGZ_WINSOR_SIGMA_OVERRIDE;
            std::array<float, N_DIMS> newShrinkageMin = SHRINKAGE_SCALE_MIN;

            newStateWinsorSigma = fw.value("state_winsor_sigma", newStateWinsorSigma);
            if (fw.contains("dims") && fw["dims"].is_array()) {
                for (const auto& d : fw["dims"]) {
                    const size_t idx = d.value("index", N_DIMS);
                    if (idx >= N_DIMS) { continue; }
                    newDimWinsor[idx] = d.value("dim_winsor_sigma_override", newDimWinsor[idx]);
                    newLogzWinsor[idx] = d.value("logz_winsor_sigma_override", newLogzWinsor[idx]);
                    newShrinkageMin[idx] = d.value("shrinkage_scale_min", newShrinkageMin[idx]);
                }
            }

            STATE_WINSOR_SIGMA = newStateWinsorSigma;
            DIM_WINSOR_SIGMA_OVERRIDE = newDimWinsor;
            LOGZ_WINSOR_SIGMA_OVERRIDE = newLogzWinsor;
            SHRINKAGE_SCALE_MIN = newShrinkageMin;
            configLoadStatus = ConfigLoadStatus::LOADED_FROM_FILE;
        } catch (const std::exception& e) {
            configLoadStatus = ConfigLoadStatus::PARSE_FAILED;
            Logger::getInstance().log(
                std::string("FeatureScaler: config PARSE_FAILED at ") + path + " (" + e.what() +
                ") -- using compiled defaults");
        }
    }

    static constexpr size_t RANK_WINDOW = 500;                   ///< Max rolling window (scratch array sizing + warmup gate)
    static constexpr size_t EXPECTED_LOGZ_DIMS = 3;
    static constexpr size_t RECALIBRATION_INTERVAL = 5000;       ///< Re-examine adaptive floors every N samples
    static constexpr size_t CARRY_DECAY_HALFLIFE = 200;          ///< Carry-forward exponential decay half-life (samples)

    /// Dim 5 (lempel_ziv) static scaler — bypasses rolling MAD.
    /// LZ76 on n=64 binary string produces ~10 discrete values (step ≈ 0.094).
    /// MAD(identical values) = 0.0 → guaranteed carry-forward death spiral.
    /// Static center/scale keeps the signal alive without zero-denominator risk.
    static constexpr size_t DIM_LZ_INDEX = 5;
    static constexpr float LZ_STATIC_CENTER = 0.5f;              ///< Theoretical mean of LZ76 for n=64
    static constexpr float LZ_STATIC_SCALE = 0.25f;              ///< Maps [0,1] → [-2,+2] z-score range

    /// Dim 13/14 (recurrence/fractal): bounded structural channels.
    /// Use static bounded scaling to avoid MAD-floor carry-forward collapse
    /// on slow/discrete regimes while preserving topology via Soft-Log-Z.
    static constexpr size_t DIM_RECURRENCE_INDEX = 13;
    static constexpr size_t DIM_FRACTAL_INDEX = 14;
    /// Recalibrated 2026-07-23 against real event_data.context (500k-sample pull):
    /// original constants assumed symmetric use of the theoretical contract range,
    /// but real data centers well off that assumption and only spans a narrow band.
    /// Recentered to empirical raw median; rescaled to 1.4826*empirical raw MAD
    /// (the same Taleb-consistent convention RobustLocation() uses for adaptive
    /// dims below), so real variation spans the intended [-2,+2] ToSoftLogZ range
    /// instead of a narrow off-center sliver. See docs/hmm/STUDENT_T_HMM_RUNBOOK.md
    /// (lbrnet) item 4b and logs/rc_gemini.log CLAUDE_BRIEF_024/025 for derivation.
    /// Dim 13 RE-derived 2026-08-13 (final-review Finding 1). The 2026-07-23
    /// constants (center 0.329, scale 0.083) were calibrated against the OLD
    /// RQA epsilon heuristic max(range*0.1, std*0.5). That heuristic was
    /// replaced by a fixed-target-recurrence-rate selector (RQAEpsilonSelector.h;
    /// target 0.05 full-matrix, line-of-identity included) and the raw dim moved
    /// to a ~0.033-0.15 band, so the old constants mapped every production value
    /// to z ~= -3.2 -- a Soft-Log-Z p01..p99 span of only 0.215, i.e. the
    /// dimension-collapse pathology this initiative exists to remove.
    /// Methodology (same as Task 7's kurtosis re-derivation: real data, not
    /// synthetic): tools/rqa_recurrence_calibration.cpp runs the shipped
    /// CalculateRecurrenceRate logic verbatim (n=30 -- TripleScreen2's
    /// slow_window_n is deterministically 30; bar-gated 200-bar epsilon
    /// recalibration) over 18,742 real 60-minute MES bars resampled from
    /// lbrnet/data/raw/mes_ripple_15m.parquet (2023-06-04 to 2026-08-10).
    /// Result: median 0.04667, MAD 0.00667 -> 1.4826*MAD = 0.00988. Stable
    /// across split-halves and 3 yearly sub-periods (median 0.0444-0.0489,
    /// scale 0.0099-0.0132). Verified non-degenerate: Soft-Log-Z p01..p99 span
    /// widens from 0.215 (old) to 2.80 (new), 34 distinct scaled values.
    static constexpr float RECURRENCE_STATIC_CENTER = 0.0467f;   ///< empirical median (was 0.329)
    static constexpr float RECURRENCE_STATIC_SCALE = 0.0099f;    ///< 1.4826*raw MAD (was 0.083)
    static constexpr float FRACTAL_STATIC_CENTER = 1.289f;       ///< empirical median (was 1.5)
    static constexpr float FRACTAL_STATIC_SCALE = 0.101f;        ///< 1.4826*raw MAD (was 0.25)

    /// Per-dim rolling window depths — tuned to decorrelation characteristics.
    /// Short-memory features (bounded, fast-decorrelating) use shorter windows.
    /// Long-memory features (structural, slow-decorrelating) use longer windows.
    inline static constexpr std::array<size_t, N_DIMS> DIM_WINDOW_SIZE = {
        500,  //  0  log_variance_ratio    medium memory
        300,  //  1  burstiness_index      bursty, shorter
        500,  //  2  relative_range        LOGZ, stable
        300,  //  3  correction_action     regime-reactive
        500,  //  4  vol_convexity         LOGZ, stable
        150,  //  5  lempel_ziv            bounded [0,1], static scaler (MAD-immune)
        200,  //  6  hurst_exponent        bounded ~[0,1], moderate
        300,  //  7  micro_asymmetry       order flow, bursty
        500,  //  8  fisher_info           geometry, stable
        500,  //  9  tail_index            Hill needs depth
        300,  // 10  skewness              regime-reactive
        300,  // 11  amihud_illiquidity    regime-sensitive
        500,  // 12  liq_fragility         LOGZ, stable
        200,  // 13  recurrence_rate       structural, periodic
        200,  // 14  fractal_dim           structural, slow
        300,  // 15  mean_rev_z            medium memory
    };

    /// Per-dimension adaptive scaling calibration.
    /// Calibrated at warmup and periodically recalibrated every RECALIBRATION_INTERVAL.
    struct DimCalibration {
        float minScaleFloor = ABSOLUTE_FLOOR; ///< Adaptive MAD-scale floor (robust)
        float lastValidScaled = 0.0f;         ///< Carry-forward cache
        bool  hasValidScaled = false;         ///< Guard: no carry-forward before first real value
        size_t carryForwardCount = 0;         ///< Consecutive carry-forward samples (Shannon decay)
    };

    inline static constexpr std::array<ScaleMode, N_DIMS> SCALE_MODE_MAP = {
        ScaleMode::SOFTLOGZ, // 0
        ScaleMode::SOFTLOGZ, // 1
        ScaleMode::LOGZ, // 2
        ScaleMode::SOFTLOGZ, // 3
        ScaleMode::LOGZ, // 4
        ScaleMode::SOFTLOGZ, // 5
        ScaleMode::SOFTLOGZ, // 6
        ScaleMode::SOFTLOGZ, // 7
        ScaleMode::SOFTLOGZ, // 8
        ScaleMode::SOFTLOGZ, // 9
        ScaleMode::SOFTLOGZ, // 10
        ScaleMode::SOFTLOGZ, // 11
        ScaleMode::LOGZ, // 12
        ScaleMode::SOFTLOGZ, // 13
        ScaleMode::SOFTLOGZ, // 14
        ScaleMode::SOFTLOGZ  // 15
    };

    // Rolling FIFO buffers by mode.
    // Fixed-capacity ring buffers (docs/superpowers/specs/2026-08-07-
    // contextmanager-ring-buffer-dod-design.md) -- zero heap allocation for
    // the buffer's lifetime, replacing std::deque's ongoing chunk churn as
    // the window slides. Capacity = RANK_WINDOW + 1 for headroom: push_back
    // then conditionally pop_front (below) transiently holds one more
    // element than the per-dim logical window between those two statements.
    std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> stateBuffers; // SOFTLOGZ dims use raw rolling z-score
    std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> logBuffers;
    // Latest rolling robust stats for diagnostics/telemetry.
    std::array<float, N_DIMS> latestLogMedian = {};
    std::array<float, N_DIMS> latestLogScale = {};  ///< MAD × 1.4826

    std::array<DimCalibration, N_DIMS> calibration = {};  ///< Per-dim adaptive state

    /// Per-dim result of the last Calibrate()/Recalibrate() dominance check:
    /// fraction of the rolling window occupied by its single most-frequent
    /// exact value. Exposed (not logged here -- this header has zero
    /// ACSIL/Logger dependency by design) so a caller with Logger access can
    /// flag a sentinel-collapse signature
    /// (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md D2).
    ///
    /// SEMANTIC SHIFT (2026-08-13, final-review pass): Task 1's consecutive-identical
    /// dedupe (`3d5f9af`) drops repeats BEFORE they reach the rolling buffer this
    /// diagnostic scans, so this ratio now measures dominance among DISTINCT
    /// observations, no longer fraction-of-time-held. The 0.30 ALERT threshold
    /// documented in that spec was calibrated on the OLD (pre-dedupe) meaning and
    /// is therefore harder to trip now; reconsider it if it starts firing
    /// unexpectedly post-dedupe (a post-dedupe hit is a stronger signal than a
    /// pre-dedupe one -- it means genuinely distinct samples keep landing on the
    /// same exact value, not merely that a held value repeated).
    std::array<float, N_DIMS> dominanceRatio = {};

    /// Long-horizon EWMA scale reference (DIM_MACRO_SHRINKAGE_INDEX only;
    /// harmless zero-cost slot for other dims). Seeded from bufScale at
    /// Calibrate() time, then evolves continuously per-tick via
    /// MACRO_SCALE_DECAY -- deliberately NOT reseeded at Recalibrate(), since
    /// a periodic snapshot is exactly the "loses memory across regime
    /// persistence" property this exists to avoid.
    std::array<float, N_DIMS> macroScaleEwma = {};

    /// Diagnostic-only: the raw pre-winsorization z computed on the last live
    /// (non-carry-forward) call, before ToSoftLogZ/ENERGY_WINSOR clamp it.
    /// Same rationale as latestLogMedian/latestLogScale above -- the public
    /// result[] is always already clamped to +-6, so an over-amplified scale
    /// is otherwise invisible from outside the class.
    std::array<float, N_DIMS> lastRawZ = {};

    /// Diagnostic-only (DIM_MACRO_SHRINKAGE_INDEX only; harmless unused slots
    /// for other dims): the live local madScale and shrinkage weight w behind
    /// the last scale_effective blend, otherwise reconstructable only by
    /// re-deriving them from lastRawZ/macroScaleEwma, which isn't possible in
    /// general (two unknowns, one equation). Added to trace dim3's remaining
    /// event clusters with the same precision D4's investigation used.
    std::array<float, N_DIMS> lastLocalMad = {};
    std::array<float, N_DIMS> lastShrinkageWeight = {};

    bool calibrated = false;                               ///< Set once at warmup completion

    bool modeMapValidated = false;
    bool modeMapIsValid = false;
    size_t sampleCount = 0;
    bool warmedUp = false;

    bool EnsureModeMapValid() {
        if (modeMapValidated) {
            return modeMapIsValid;
        }

        size_t stateCount = 0;
        size_t logzCount = 0;
        for (ScaleMode mode : SCALE_MODE_MAP) {
            if (mode == ScaleMode::SOFTLOGZ) {
                ++stateCount;
            } else if (mode == ScaleMode::LOGZ) {
                ++logzCount;
            }
        }

        modeMapIsValid = ((stateCount + logzCount) == N_DIMS) && (logzCount == EXPECTED_LOGZ_DIMS);
        modeMapValidated = true;
        assert(modeMapIsValid && "FeatureScaler SCALE_MODE_MAP must match expected LOGZ/SOFTLOGZ partition");
        return modeMapIsValid;
    }

    static float ToLogEnergy(float rawValue) {
        const float magnitude = std::max(std::abs(rawValue), LOG_EPS);
        return std::log1p(magnitude * LOG_BPS);
    }

    static float ToSoftLogZ(float z, float winsorSigma = STATE_WINSOR_SIGMA) {
        const float zw = std::clamp(z, -winsorSigma, winsorSigma);
        return std::copysign(std::log1p(std::abs(zw)), zw);
    }

    /// Robust location + scale: median and MAD × 1.4826 (Taleb-consistent).
    /// Uses std::nth_element on stack scratch — O(n) average, zero heap allocation.
    /// Returns {median, madScale}. If fewer than 5 samples, returns {0, 0}.
    static std::pair<float, float> RobustLocation(const RingBuffer<float, RANK_WINDOW + 1>& buf) {
        const int n = static_cast<int>(buf.size());
        if (n < 5) return {0.0f, 0.0f};

        std::array<float, RANK_WINDOW> scratch;  // Sized to max window (500)
        for (int j = 0; j < n; ++j) scratch[static_cast<size_t>(j)] = buf[static_cast<size_t>(j)];
        const int mid = n / 2;

        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float median = scratch[static_cast<size_t>(mid)];

        for (int j = 0; j < n; ++j) {
            scratch[static_cast<size_t>(j)] = std::abs(buf[static_cast<size_t>(j)] - median);
        }
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float madScale = scratch[static_cast<size_t>(mid)] * 1.4826f;

        return {median, madScale};
    }

    /// Fraction of `buf` occupied by its single most-frequent exact value.
    /// O(n log n) via sort-then-scan-longest-equal-run on the same stack
    /// scratch RobustLocation() already uses -- only called from
    /// Calibrate()/Recalibrate() (warmup completion + every
    /// RECALIBRATION_INTERVAL samples), never per-tick. Pure, native-testable.
    static float ComputeValueDominance(const RingBuffer<float, RANK_WINDOW + 1>& buf) {
        const int n = static_cast<int>(buf.size());
        if (n == 0) return 0.0f;
        std::array<float, RANK_WINDOW> scratch;
        for (int j = 0; j < n; ++j) scratch[static_cast<size_t>(j)] = buf[static_cast<size_t>(j)];
        std::sort(scratch.begin(), scratch.begin() + n);
        int maxRun = 1;
        int currentRun = 1;
        for (int j = 1; j < n; ++j) {
            if (scratch[static_cast<size_t>(j)] == scratch[static_cast<size_t>(j - 1)]) {
                ++currentRun;
                maxRun = std::max(maxRun, currentRun);
            } else {
                currentRun = 1;
            }
        }
        return static_cast<float>(maxRun) / static_cast<float>(n);
    }

    /// Shared shrinkage-blend z computation (D8), used by both the SOFTLOGZ
    /// and LOGZ paths for any dim with a nonzero SHRINKAGE_SCALE_MIN entry --
    /// extracted 2026-08-14 (Task 4's dim4 follow-up) when dim4 (a LOGZ dim)
    /// independently needed the identical mechanism dim3/dim9/dim0/dim7/dim6
    /// already had, rather than duplicating the blend logic a second time.
    /// Caller must have already handled the macroScaleEwma[i]<=0.0f
    /// bootstrap-guard case (both paths' guard response differs slightly --
    /// SOFTLOGZ also zeroes lastRawZ[i] since LOGZ dims don't populate that
    /// diagnostic array -- so that check stays at each call site, not here).
    /// `current` is the value being scaled: the raw state value for SOFTLOGZ,
    /// the log-energy value (post-ToLogEnergy) for LOGZ; `median`/`madScale`
    /// are that path's own RobustLocation() output on its own rolling buffer.
    float ComputeShrinkageZ(size_t i, float current, float median, float madScale) {
        // Compression ratio in log-space, computed against the PRE-update
        // macro reference: how far has the live local MAD shrunk relative to
        // its own long-horizon anchor? ratio == midpoint -> w == 0.5;
        // ratio >> midpoint (healthy, local as/more dispersed than its own
        // history) -> w -> 1 (trust local); ratio << midpoint (compressed
        // regime) -> w -> 0 (shrink toward macroScaleEwma).
        const float macroRef = std::max(macroScaleEwma[i], ABSOLUTE_FLOOR);
        const float compressionRatio = madScale / macroRef;
        const float logRatio = std::log(std::max(compressionRatio, ABSOLUTE_FLOOR)
            / SHRINKAGE_RATIO_MIDPOINT);
        const float w = 1.0f / (1.0f + std::exp(-SHRINKAGE_STEEPNESS * logRatio));
        lastLocalMad[i] = madScale;
        lastShrinkageWeight[i] = w;

        // Ratchet: gate the anchor's own update rate by w. A plain, always-on
        // EWMA erodes right along with madScale during a sufficiently long
        // quiet regime (measured, dim3: 0.093 -> 0.031 over ~35k samples
        // leading into the known pathological event, ~3x decay despite a
        // ~13.9k-sample half-life) -- which undermines the whole point of
        // having a "long-horizon, regime-resistant" anchor. Scaling the
        // update weight by w means the anchor only tracks fresh data during
        // periods it currently judges healthy, and nearly freezes while
        // local is judged compressed -- same principle as GARCH's omega
        // being a stable, broadly-estimated floor rather than something that
        // keeps drifting downward with every quiet spell.
        const float updateWeight = (1.0f - MACRO_SCALE_DECAY) * w;
        macroScaleEwma[i] = std::max(
            (1.0f - updateWeight) * macroScaleEwma[i] + updateWeight * std::abs(current - median),
            SHRINKAGE_SCALE_MIN[i]);

        const float scaleEffective = std::max(
            w * madScale + (1.0f - w) * macroRef, ABSOLUTE_FLOOR);
        return (current - median) / scaleEffective;
    }

    /// Feed raw observation, return hybrid-scaled values.
    /// Safe from bar 1: returns neutral defaults until warmup.
    std::array<float, N_DIMS> UpdateAndNormalize(const std::array<float, N_DIMS>& raw) {
        std::array<float, N_DIMS> result;
        result.fill(0.0f);  // Neutral for signed topology-preserving space.

        if (!EnsureModeMapValid()) {
            return result;
        }

        // ── Step 1: Update rolling buffers by mode (per-dim window depth) ──
        // Dedupe-at-ingestion: a value identical to the window's most recent entry
        // carries zero marginal Shannon information (H(X_t|X_{t-1})=0 for an exact
        // repeat) and must not be treated as a fresh independent draw by the
        // median/MAD estimator below -- see docs/superpowers/specs/
        // 2026-08-13-observation-vector-institutional-elevation-spec.md Unit 1.
        for (size_t i = 0; i < N_DIMS; ++i) {
            const size_t winSize = DIM_WINDOW_SIZE[i];
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                auto& buf = stateBuffers[i];
                if (buf.empty() || buf.back() != raw[i]) {
                    buf.push_back(raw[i]);
                    if (buf.size() > winSize) {
                        buf.pop_front();
                    }
                }
            } else {
                const float logValue = ToLogEnergy(raw[i]);
                auto& buf = logBuffers[i];
                if (buf.empty() || buf.back() != logValue) {
                    buf.push_back(logValue);
                    if (buf.size() > winSize) {
                        buf.pop_front();
                    }
                }
            }
        }

        ++sampleCount;

        if (sampleCount == 1) {
            latestLogMedian.fill(0.0f);
            latestLogScale.fill(0.0f);
            return result;
        }

        // ── Step 2: Scale by mode (SOFTLOGZ or LOGZ) ─────────────────────
        for (size_t i = 0; i < N_DIMS; ++i) {
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& stateBuf = stateBuffers[i];
                if (stateBuf.empty()) {
                    result[i] = 0.0f;
                    continue;
                }

                const float current = stateBuf.back();

                // ── Dim 5 (lempel_ziv): Static global scaler ──────────────
                // LZ76 output is discrete (~10 levels for n=64).  MAD of
                // identical discrete values is exactly 0.0, which triggers
                // the carry-forward death spiral.  Static center/scale
                // keeps the signal alive without zero-denominator risk.
                if (i == DIM_LZ_INDEX) {
                    const float z = (current - LZ_STATIC_CENTER) / LZ_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                // Structural bounded channels use static bounded scaling.
                if (i == DIM_RECURRENCE_INDEX) {
                    const float z = (current - RECURRENCE_STATIC_CENTER) / RECURRENCE_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                if (i == DIM_FRACTAL_INDEX) {
                    const float z = (current - FRACTAL_STATIC_CENTER) / FRACTAL_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                const auto [median, madScale] = RobustLocation(stateBuf);

                // Shrinkage-blended scale instead of the binary floor-gate
                // below, for any dim with a nonzero SHRINKAGE_SCALE_MIN entry
                // -- see that array's doc comment.
                if (SHRINKAGE_SCALE_MIN[i] > 0.0f) {
                    // Bootstrap guard: before Calibrate() has ever seeded
                    // macroScaleEwma, it sits at its zero-initialized default,
                    // so max(macroScaleEwma, ABSOLUTE_FLOOR) would clamp to the
                    // bare 1e-8 floor and any nonzero raw value divided by that
                    // explodes (measured: |z| ~ 6.8e7 on real data before the
                    // first Calibrate() fires). The old floor-gate handled this
                    // for free -- madScale==0 always routed to carry-forward,
                    // which returns a safe 0.0f before hasValidScaled is ever
                    // true. Replicate that safety explicitly here instead of
                    // attempting a blend with no real anchor to blend against.
                    if (macroScaleEwma[i] <= 0.0f) {
                        result[i] = 0.0f;
                        lastRawZ[i] = 0.0f;
                        calibration[i].carryForwardCount = 0;
                        continue;
                    }

                    calibration[i].carryForwardCount = 0;
                    const float z = ComputeShrinkageZ(i, current, median, madScale);
                    lastRawZ[i] = z;
                    const float shrinkWinsorSigma = (DIM_WINSOR_SIGMA_OVERRIDE[i] > 0.0f)
                        ? DIM_WINSOR_SIGMA_OVERRIDE[i] : STATE_WINSOR_SIGMA;
                    result[i] = ToSoftLogZ(z, shrinkWinsorSigma);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    continue;
                }

                if (madScale < calibration[i].minScaleFloor) {
                    // Shannon decay: carry-forward value decays toward neutral
                    calibration[i].carryForwardCount++;
                    if (calibration[i].hasValidScaled) {
                        const float decay = std::exp2f(
                            -static_cast<float>(calibration[i].carryForwardCount)
                            / static_cast<float>(CARRY_DECAY_HALFLIFE));
                        result[i] = calibration[i].lastValidScaled * decay;
                    } else {
                        result[i] = 0.0f;
                    }
                    continue;
                }

                calibration[i].carryForwardCount = 0;  // Reset on live signal
                const float z = (current - median) / madScale;
                lastRawZ[i] = z;
                const float winsorSigma = (DIM_WINSOR_SIGMA_OVERRIDE[i] > 0.0f)
                    ? DIM_WINSOR_SIGMA_OVERRIDE[i] : STATE_WINSOR_SIGMA;
                result[i] = ToSoftLogZ(z, winsorSigma);
                calibration[i].lastValidScaled = result[i];
                calibration[i].hasValidScaled = true;
                continue;
            }

            const auto& logBuf = logBuffers[i];
            if (logBuf.empty()) {
                result[i] = 0.0f;
                latestLogMedian[i] = 0.0f;
                latestLogScale[i] = 0.0f;
                continue;
            }

            const float currentLog = logBuf.back();
            const auto [median, madScale] = RobustLocation(logBuf);

            latestLogMedian[i] = median;
            latestLogScale[i] = madScale;

            // Shrinkage-blended scale, same mechanism/array as the SOFTLOGZ
            // path above (D8, generalized to LOGZ 2026-08-14 when dim4
            // independently showed the identical scale-collapse signature --
            // see SHRINKAGE_SCALE_MIN's doc comment and ComputeShrinkageZ()).
            if (SHRINKAGE_SCALE_MIN[i] > 0.0f) {
                if (macroScaleEwma[i] <= 0.0f) {
                    result[i] = 0.0f;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }
                calibration[i].carryForwardCount = 0;
                const float zLog = ComputeShrinkageZ(i, currentLog, median, madScale);
                const float energyWinsorSigma = (LOGZ_WINSOR_SIGMA_OVERRIDE[i] > 0.0f)
                    ? LOGZ_WINSOR_SIGMA_OVERRIDE[i] : ENERGY_WINSOR_SIGMA;
                result[i] = std::clamp(zLog, -energyWinsorSigma, energyWinsorSigma);
                calibration[i].lastValidScaled = result[i];
                calibration[i].hasValidScaled = true;
                continue;
            }

            if (madScale < calibration[i].minScaleFloor) {
                // Shannon decay: carry-forward value decays toward neutral
                calibration[i].carryForwardCount++;
                if (calibration[i].hasValidScaled) {
                    const float decay = std::exp2f(
                        -static_cast<float>(calibration[i].carryForwardCount)
                        / static_cast<float>(CARRY_DECAY_HALFLIFE));
                    result[i] = calibration[i].lastValidScaled * decay;
                } else {
                    result[i] = 0.0f;
                }
                continue;
            }

            calibration[i].carryForwardCount = 0;  // Reset on live signal
            const float zLog = (currentLog - median) / madScale;
            const float energyWinsorSigma = (LOGZ_WINSOR_SIGMA_OVERRIDE[i] > 0.0f)
                ? LOGZ_WINSOR_SIGMA_OVERRIDE[i] : ENERGY_WINSOR_SIGMA;
            result[i] = std::clamp(zLog, -energyWinsorSigma, energyWinsorSigma);
            calibration[i].lastValidScaled = result[i];
            calibration[i].hasValidScaled = true;
        }

        // Warmup gate: RANK_WINDOW samples for full rolling buffer depth
        if (!warmedUp && sampleCount >= RANK_WINDOW) {
            warmedUp = true;
            if (!calibrated) {
                Calibrate();
            }
        }

        // Rolling recalibration: update adaptive floors periodically (Mandelbrot regime fix)
        if (warmedUp && (sampleCount % RECALIBRATION_INTERVAL == 0)) {
            Recalibrate();
        }

        return result;
    }

    /// Snapshot per-dim MAD-scale from burn-in buffers → adaptive floors.
    /// Called once when warmedUp flips to true.
    void Calibrate() {
        for (size_t i = 0; i < N_DIMS; ++i) {
            float bufScale = 0.0f;
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& buf = stateBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            }
            const float floorToUse = (i == DIM_AMIHUD_INDEX) ? AMIHUD_ABSOLUTE_FLOOR : ABSOLUTE_FLOOR;
            calibration[i].minScaleFloor = std::max(floorToUse, bufScale * RELATIVE_FLOOR_FRACTION);
            // One-time seed; the live UpdateAndNormalize() branch evolves it
            // per-tick thereafter for any dim with shrinkage enabled. Floored
            // only for shrinkage-enabled dims -- SHRINKAGE_SCALE_MIN[i] is
            // empirically derived per-dim, 0.0f (no-op floor) for every
            // other dim (macroScaleEwma is otherwise an unused, harmless
            // slot for them).
            macroScaleEwma[i] = (SHRINKAGE_SCALE_MIN[i] > 0.0f)
                ? std::max(bufScale, SHRINKAGE_SCALE_MIN[i]) : bufScale;
        }
        calibrated = true;
    }

    /// Periodic recalibration: EMA-blend current MAD-scale into adaptive floors.
    /// Prevents the "Great Moderation" trap — floors track regime changes.
    /// Called every RECALIBRATION_INTERVAL samples after warmup.
    void Recalibrate() {
        for (size_t i = 0; i < N_DIMS; ++i) {
            float bufScale = 0.0f;
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& buf = stateBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            }
            const float floorToUse = (i == DIM_AMIHUD_INDEX) ? AMIHUD_ABSOLUTE_FLOOR : ABSOLUTE_FLOOR;
            const float newFloor = std::max(floorToUse, bufScale * RELATIVE_FLOOR_FRACTION);
            // EMA blend: 70% old + 30% new — conservative, prevents floor whiplash
            calibration[i].minScaleFloor = 0.7f * calibration[i].minScaleFloor + 0.3f * newFloor;
        }
    }

    void Reset() {
        for (auto& buf : stateBuffers) buf.clear();
        for (auto& buf : logBuffers) buf.clear();
        latestLogMedian.fill(0.0f);
        latestLogScale.fill(0.0f);
        calibration = {};
        dominanceRatio.fill(0.0f);
        macroScaleEwma.fill(0.0f);
        lastRawZ.fill(0.0f);
        lastLocalMad.fill(0.0f);
        lastShrinkageWeight.fill(0.0f);
        calibrated = false;
        modeMapValidated = false;
        modeMapIsValid = false;
        sampleCount = 0;
        warmedUp = false;
    }
};
