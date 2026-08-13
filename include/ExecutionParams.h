#pragma once
// ── P-AER Execution Parameters ──────────────────────────────────────────────
// Runtime-configurable risk gates and tuning knobs.
// Loaded from JSON at Init(). File is MANDATORY — RiskManager will halt study
// startup if file is missing or unparseable. No silent fallback to defaults.
// Pattern follows HMMRiskPolicy (RiskManager.cpp).

#include <string>

struct ExecutionParams {
    enum class LoadStatus {
        DEFAULTS,
        LOADED_FROM_FILE,
        FILE_MISSING,
        PARSE_FAILED
    };

    static constexpr const char* kDefaultPath = "C:/Trading/config/execution_params.json";

    LoadStatus loadStatus = LoadStatus::DEFAULTS;

    // ── Elder risk limits ──
    double elder2PctRuleFrac          = 0.02;
    double maxRiskPerTradeFrac        = 0.02;
    double elderPortfolioHeatFrac     = 0.06;
    double elder6PctRuleFrac          = 0.06;
    int    elderLossStreakBreaker      = 2;
    int    maxTradesPerDay            = 4;
    int    elderRevengeCooloffMin     = 30;
    double maxDailyWinPct             = 0.05;
    double dailyWinWarningPct         = 0.03;
    int    rapidFireLimitPerMin       = 2;

    // ── Pattern quality thresholds ──
    float  minPatternQualityBase      = 0.6f;
    float  probationQualityFloor      = 0.8f;
    float  raschkeOpeningQualityFloor = 0.70f;
    float  raschkeLunchDeadzoneFloor  = 0.80f;
    float  raschkeAfternoonQualityFloor = 0.65f;

    // ── Position defense thresholds (holding score gates) ──
    float  liveToxicScoreThreshold    = 0.65f;
    float  liveHostileScoreThreshold  = 0.85f;
    float  liveForceFlatConfidenceCap = 0.70f;

    // ── Latency breaker policy (microseconds) ──
    long   regimeLatencyOptimalUs     = 8000;
    long   regimeLatencyBreakerUs     = 25000;
    float  regimeLatencyMinFactor     = 0.60f;
    bool   latencyGateObserveOnly     = true;

    // ── Elder drawdown thresholds ──
    double elderDrawdown10PctSizing75 = 0.10;
    double elderDrawdown15PctSizing50 = 0.15;
    double drawdownHalt               = 0.25;

    // ── Taleb kurtosis crisis gate (hysteresis) ──
    // Enter/Exit percentile-matched to the pre-Task-6 moment-based scale's
    // original intent (Enter: was 5.0, P62.7 -> 1.5650; Exit: was 3.0, P38.7
    // -> 1.3809) -- see tools/analyze_kurtosis_threshold_migration.py, run
    // 2026-08-13 (Task 7, .superpowers/sdd/2026-08-13-observation-vector-
    // institutional-elevation/task-7-report.md). CrisisCeiling is a risk-
    // multiplier cap (fraction of normal size), not a kurtosis-scale value
    // -- unaffected by the Moors-kurtosis swap, left as-is.
    float  talebKurtosisCrisisEnter   = 1.5650f;
    float  talebKurtosisCrisisExit    = 1.3809f;
    float  talebKurtosisCrisisCeiling = 0.35f;

    // ── Hard-gate regime critical cutoffs (single source of truth shared by
    //    EvaluateHardGates and ValidateOrder's regime filter) ──
    // Percentile-matched: was 15.0 (old moment-based scale), P91.9 of the
    // historical distribution -- see tools/analyze_kurtosis_threshold_
    // migration.py, run 2026-08-13 (Task 7).
    float  talebKurtosisHaltThreshold = 2.0064f;  // kurtosis above this = flash-crash block
    // ENTROPY SCALE NOTE (2026-08-13, final-review Finding 7): GetShannonEntropy()
    // now subtracts a Miller-Madow bias term, shifting entropy DOWN ~4% at the
    // steady-state window (N=50), up to ~20% during warmup (N=10). This fraction
    // was evaluated and deliberately NOT re-derived: the shift is an order of
    // magnitude smaller than the kurtosis 3.0 -> 1.233 change directly above
    // (which DID get a full empirical re-derivation, Task 7), and it moves the
    // halt gate conservatively (harder to trip a chaos block). Full rationale at
    // InformationEngine.h's GetShannonEntropy() doc comment.
    float  shannonEntropyHaltFrac     = 0.90f;  // fraction of kShannonMaxEntropyBits = chaos block

    // ── Shared TRAP contract (cross-runtime parity with Python) ──
    std::string trapContractVersion   = "v1";
    std::string trapRefactorMode      = "fsm_authoritative_recompute";
    float  trapCautionThreshold       = 0.45f;
    float  trapEmitThreshold          = 0.65f;
    float  trapHardStopThreshold      = 0.80f;
    float  trapStructuralFloor        = 0.20f;
    float  trapEntryScoreMin          = 80.0f;
    float  trapMfeMaeRatioMax         = 0.20f;
    float  trapStopStressMin          = 0.90f;
    float  trapWeightElderRaschke     = 0.35f;
    float  trapWeightShannon          = 0.10f;
    float  trapWeightMandelbrot       = 0.10f;
    float  trapWeightTaleb            = 0.15f;
    float  trapWeightPareto           = 0.10f;
    float  trapWeightHmm              = 0.20f;
    float  trapReliabilityMin         = 0.70f;
    float  trapReliabilityMax         = 1.15f;

    // ── TDE shadow-mode (PAER §9 Phase 1: observation only, no behavioral change) ──
    bool   tdeShadowEnabled           = true;
    float  tdeConvictionFloor         = 0.60f;
    int    tdePositionCap             = 10;

    // ── Meta / identity ──
    std::string paramSetId            = "default";

    // ── Diagnostic mode (P-AER §7.5: evaluate ALL gates, record full-chain verdicts) ──
    bool   diagnosticMode             = false;

    // ── Load from JSON (override only keys present in file) ──
    void LoadFromFile(const std::string& path = kDefaultPath) {
        std::ifstream in(path);
        if (!in.is_open()) {
            loadStatus = LoadStatus::FILE_MISSING;
            Logger::getInstance().log(
                "ExecutionParams: Using compiled defaults (file not found at " + path + ")");
            return;
        }

        try {
            const std::string payload((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            nlohmann::json j = nlohmann::json::parse(payload);

            // Elder risk limits
            elder2PctRuleFrac      = j.value("elder_2pct_rule_frac",      elder2PctRuleFrac);
            maxRiskPerTradeFrac    = j.value("max_risk_per_trade_frac",   maxRiskPerTradeFrac);
            elderPortfolioHeatFrac = j.value("elder_portfolio_heat_frac", elderPortfolioHeatFrac);
            elder6PctRuleFrac      = j.value("elder_6pct_rule_frac",      elder6PctRuleFrac);
            elderLossStreakBreaker = j.value("elder_loss_streak_breaker",  elderLossStreakBreaker);
            maxTradesPerDay        = j.value("max_trades_per_day",        maxTradesPerDay);
            elderRevengeCooloffMin = j.value("elder_revenge_cooloff_min", elderRevengeCooloffMin);
            maxDailyWinPct         = j.value("max_daily_win_pct",         maxDailyWinPct);
            dailyWinWarningPct     = j.value("daily_win_warning_pct",     dailyWinWarningPct);
            rapidFireLimitPerMin   = j.value("rapid_fire_limit_per_min",  rapidFireLimitPerMin);

            // Pattern quality thresholds
            minPatternQualityBase      = j.value("min_pattern_quality_base",      minPatternQualityBase);
            probationQualityFloor      = j.value("probation_quality_floor",       probationQualityFloor);
            raschkeOpeningQualityFloor = j.value("raschke_opening_quality_floor", raschkeOpeningQualityFloor);
            raschkeLunchDeadzoneFloor  = j.value("raschke_lunch_deadzone_floor",  raschkeLunchDeadzoneFloor);
            raschkeAfternoonQualityFloor = j.value("raschke_afternoon_quality_floor", raschkeAfternoonQualityFloor);
            liveToxicScoreThreshold    = j.value("live_toxic_score_threshold", liveToxicScoreThreshold);
            liveHostileScoreThreshold  = j.value("live_hostile_score_threshold", liveHostileScoreThreshold);
            liveForceFlatConfidenceCap = j.value("live_force_flat_confidence_cap", liveForceFlatConfidenceCap);

            // Guardrail: hostile threshold must remain above toxic threshold.
            if (liveHostileScoreThreshold <= liveToxicScoreThreshold) {
                liveHostileScoreThreshold = std::max(liveHostileScoreThreshold, liveToxicScoreThreshold + 0.05f);
            }

            // Latency breaker policy
            regimeLatencyOptimalUs = j.value("regime_latency_optimal_us", regimeLatencyOptimalUs);
            regimeLatencyBreakerUs = j.value("regime_latency_breaker_us", regimeLatencyBreakerUs);
            regimeLatencyMinFactor = j.value("regime_latency_min_factor", regimeLatencyMinFactor);
            latencyGateObserveOnly = j.value("latency_gate_observe_only", latencyGateObserveOnly);

            // Elder drawdown thresholds
            elderDrawdown10PctSizing75 = j.value("elder_drawdown_10pct_sizing_75", elderDrawdown10PctSizing75);
            elderDrawdown15PctSizing50 = j.value("elder_drawdown_15pct_sizing_50", elderDrawdown15PctSizing50);
            drawdownHalt               = j.value("drawdown_halt",                  drawdownHalt);

            // ── Taleb kurtosis gates (STRICT validation — fail closed) ──────
            // SCALE GUARD (final-review Finding 9, 2026-08-13). Task 6 replaced
            // moment-based excess kurtosis with Moors (1988) octile kurtosis, a
            // completely different scale: CalculateRealizedKurtosis now clamps
            // its output to [0, 5] and realistically returns <= ~3, whereas the
            // old statistic routinely reached 10-20. A stale old-scale override
            // in /mnt/c/Trading/config/execution_params.json (e.g. the real
            // pre-Task-7 values `taleb_kurtosis_halt_threshold: 15.0` /
            // `taleb_kurtosis_crisis_enter: 17.33`) would therefore fail OPEN --
            // silently unreachable, so the flash-crash block never fires -- with
            // no other symptom. Task 7 restamped the live JSON, but nothing
            // stopped a stale copy being restored, and these are safety gates.
            //
            // These are ENTRY-BLOCKING / risk-tightening gates, so
            // TRADE_EXECUTION_SYSTEM.md §14.3's non-negotiable invariant
            // ("fail-closed for new entries, fail-open for protective exits")
            // requires REFUSING TO ARM on an implausible value, not quietly
            // substituting a default that may itself be wrong. Hence `throw`,
            // matching trap_config.contract_version's precedent below: the catch
            // sets loadStatus = PARSE_FAILED, which RiskManager::Initialize
            // treats as FATAL and aborts initialization on — a real deny state,
            // not a warning. The catch also restores compiled defaults so the
            // object is never left half-overridden.
            //
            // Additionally enforces TRADE_EXECUTION_SYSTEM.md §13.3's mandatory
            // hysteresis ordering for threshold-based degraders: a config whose
            // enter/exit values were swapped would pass a per-value range check
            // but is still a real bug.
            constexpr float kKurtosisMoorsClampCeiling = 5.0f;  // == CalculateRealizedKurtosis's KURT_CLAMP_HI

            talebKurtosisCrisisEnter   = j.value("taleb_kurtosis_crisis_enter",   talebKurtosisCrisisEnter);
            talebKurtosisCrisisExit    = j.value("taleb_kurtosis_crisis_exit",    talebKurtosisCrisisExit);
            // NOTE: talebKurtosisCrisisCeiling is a risk-MULTIPLIER cap (a size
            // fraction), not a kurtosis-scale value, so it is not guarded here.
            talebKurtosisCrisisCeiling = j.value("taleb_kurtosis_crisis_ceiling", talebKurtosisCrisisCeiling);

            // Hard-gate regime critical cutoffs
            talebKurtosisHaltThreshold = j.value("taleb_kurtosis_halt_threshold", talebKurtosisHaltThreshold);
            shannonEntropyHaltFrac     = j.value("shannon_entropy_halt_frac",     shannonEntropyHaltFrac);

            auto requireMoorsScale = [&](const char* key, float value) {
                if (!(value < kKurtosisMoorsClampCeiling) || value < 0.0f) {
                    throw std::runtime_error(
                        std::string(key) + " = " + std::to_string(value) +
                        " is not on the Moors octile-kurtosis scale (valid range [0,5); "
                        "CalculateRealizedKurtosis clamps its output to [0,5], so this "
                        "threshold is unreachable and the gate would never fire). This is "
                        "almost certainly a stale pre-Task-7 old-moment-scale value — "
                        "restamp the config using the Task 7 percentile mapping table.");
                }
            };
            requireMoorsScale("taleb_kurtosis_crisis_enter",   talebKurtosisCrisisEnter);
            requireMoorsScale("taleb_kurtosis_crisis_exit",    talebKurtosisCrisisExit);
            requireMoorsScale("taleb_kurtosis_halt_threshold", talebKurtosisHaltThreshold);

            // Hysteresis ordering (TRADE_EXECUTION_SYSTEM.md §13.3, "mandatory
            // for threshold-based degraders: separate enter and clear
            // thresholds"). Crisis mode is entered above `enter` and cleared
            // below `exit`, so exit must sit strictly below enter or the degrader
            // chatters / never clears. A config with the two swapped passes every
            // per-value range check and is still a real bug.
            //
            // Deliberately NOT asserted: `crisis_enter <= halt_threshold`. The
            // live tune legitimately sets crisis-enter (P93.8) ABOVE the compiled
            // halt default (P91.9) — Task 7 percentile-matching preserved that
            // pre-existing relative ordering rather than second-guessing it, so
            // an escalation-ordering assertion here would reject a valid config.
            if (!(talebKurtosisCrisisExit < talebKurtosisCrisisEnter)) {
                throw std::runtime_error(
                    "taleb_kurtosis_crisis_exit (" + std::to_string(talebKurtosisCrisisExit) +
                    ") must be strictly below taleb_kurtosis_crisis_enter (" +
                    std::to_string(talebKurtosisCrisisEnter) + ") — hysteresis ordering violated");
            }
            if (!(shannonEntropyHaltFrac > 0.0f) || shannonEntropyHaltFrac > 1.0f) {
                throw std::runtime_error(
                    "shannon_entropy_halt_frac must be a fraction in (0,1] of kShannonMaxEntropyBits, got " +
                    std::to_string(shannonEntropyHaltFrac));
            }

            // Shared trap config contract (strict validation when present)
            if (j.contains("trap_config") && j["trap_config"].is_object()) {
                const auto& trap = j["trap_config"];

                trapContractVersion = trap.value("contract_version", trapContractVersion);
                if (trapContractVersion != "v1") {
                    throw std::runtime_error("trap_config.contract_version must be 'v1'");
                }

                trapRefactorMode = trap.value("trap_refactor_mode", trapRefactorMode);
                if (trapRefactorMode != "fsm_authoritative_recompute" &&
                    trapRefactorMode != "directional_trap_passthrough_with_validation") {
                    throw std::runtime_error("trap_config.trap_refactor_mode is invalid");
                }

                trapCautionThreshold = trap.value("trap_caution_threshold", trapCautionThreshold);
                trapEmitThreshold = trap.value("trap_emit_threshold", trapEmitThreshold);
                trapHardStopThreshold = trap.value("trap_hard_stop_threshold", trapHardStopThreshold);
                trapStructuralFloor = trap.value("trap_structural_floor", trapStructuralFloor);
                trapEntryScoreMin = trap.value("trap_entry_score_min", trapEntryScoreMin);
                trapMfeMaeRatioMax = trap.value("trap_mfe_mae_ratio_max", trapMfeMaeRatioMax);
                trapStopStressMin = trap.value("trap_stop_stress_min", trapStopStressMin);

                if (trapCautionThreshold < 0.0f || trapCautionThreshold > 1.0f ||
                    trapEmitThreshold < 0.0f || trapEmitThreshold > 1.0f ||
                    trapHardStopThreshold < 0.0f || trapHardStopThreshold > 1.0f ||
                    trapStructuralFloor < 0.0f || trapStructuralFloor > 1.0f) {
                    throw std::runtime_error("trap_config threshold out of [0,1] range");
                }
                if (trapEntryScoreMin < 0.0f || trapMfeMaeRatioMax < 0.0f || trapStopStressMin < 0.0f) {
                    throw std::runtime_error("trap_config ratio/score thresholds must be non-negative");
                }

                if (!trap.contains("weights") || !trap["weights"].is_object()) {
                    throw std::runtime_error("trap_config.weights object missing");
                }
                const auto& w = trap["weights"];
                trapWeightElderRaschke = w.value("elder_raschke", trapWeightElderRaschke);
                trapWeightShannon = w.value("shannon", trapWeightShannon);
                trapWeightMandelbrot = w.value("mandelbrot", trapWeightMandelbrot);
                trapWeightTaleb = w.value("taleb", trapWeightTaleb);
                trapWeightPareto = w.value("pareto", trapWeightPareto);
                trapWeightHmm = w.value("hmm", trapWeightHmm);

                const float weightSum = trapWeightElderRaschke + trapWeightShannon + trapWeightMandelbrot +
                    trapWeightTaleb + trapWeightPareto + trapWeightHmm;
                if (std::fabs(weightSum - 1.0f) > 1e-6f) {
                    throw std::runtime_error("trap_config.weights must sum to 1.0");
                }

                if (!trap.contains("reliability_bounds") || !trap["reliability_bounds"].is_object()) {
                    throw std::runtime_error("trap_config.reliability_bounds object missing");
                }
                const auto& rb = trap["reliability_bounds"];
                trapReliabilityMin = rb.value("min", trapReliabilityMin);
                trapReliabilityMax = rb.value("max", trapReliabilityMax);
                if (trapReliabilityMin < 0.0f || trapReliabilityMax < 0.0f || trapReliabilityMax < trapReliabilityMin) {
                    throw std::runtime_error("trap_config.reliability_bounds invalid");
                }
            }

            // Meta / identity
            paramSetId = j.value("_version", paramSetId);

            // Diagnostic mode
            diagnosticMode = j.value("diagnostic_mode", diagnosticMode);

            // TDE shadow-mode
            tdeShadowEnabled    = j.value("tde_shadow_enabled",    tdeShadowEnabled);
            tdeConvictionFloor  = j.value("tde_conviction_floor",  tdeConvictionFloor);
            tdePositionCap      = j.value("tde_position_cap",      tdePositionCap);

            loadStatus = LoadStatus::LOADED_FROM_FILE;

            Logger::getInstance().log(
                "ExecutionParams: Loaded from " + path +
                " | elder_2pct=" + std::to_string(elder2PctRuleFrac) +
                " | max_trades=" + std::to_string(maxTradesPerDay) +
                " | quality_base=" + std::to_string(minPatternQualityBase) +
                " | kurtosis_enter=" + std::to_string(talebKurtosisCrisisEnter) +
                " | trap_mode=" + trapRefactorMode +
                " | trap_emit=" + std::to_string(trapEmitThreshold) +
                " | latency_observe=" + (latencyGateObserveOnly ? "true" : "false") +
                " | drawdown_halt=" + std::to_string(drawdownHalt));

        } catch (const std::exception& e) {
            // Roll back to compiled defaults. Keys are assigned in place as they
            // are read, so without this reset a throw part-way through (from a
            // trap_config validation failure, or the kurtosis scale/hysteresis
            // guards above) would leave a HALF-OVERRIDDEN object while the log
            // claimed "using compiled defaults" — the worst possible outcome for
            // a safety-gate config, and the reason those guards can safely throw
            // (final-review Finding 9, 2026-08-13). RiskManager::Initialize
            // treats PARSE_FAILED as FATAL and refuses to arm, per
            // TRADE_EXECUTION_SYSTEM.md §14.3's fail-closed-for-entries invariant.
            const std::string reason = e.what();
            *this = ExecutionParams{};
            loadStatus = LoadStatus::PARSE_FAILED;
            Logger::getInstance().log(
                "ExecutionParams: PARSE FAILED at " + path + " (" + reason +
                ") — compiled defaults restored; RiskManager will refuse to arm");
        }
    }
};
