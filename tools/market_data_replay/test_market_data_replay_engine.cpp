// test_market_data_replay_engine.cpp — characterization tests for
// MarketDataReplayEngine (Task 1: 3x TickBarAggregator wiring only, no dim
// math yet). See docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md.
//
// Build & run natively:
//   g++ -std=c++17 -Iinclude -Iinclude/generated tools/market_data_replay/test_market_data_replay_engine.cpp -o /tmp/mdr_test && /tmp/mdr_test

#include "MarketDataReplayEngine.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}

// 2024-01-15 (winter, EST, UTC-5): 18:00:00 ET = 23:00:00 UTC. Same fixture
// timestamps as tests/cpp/test_tick_bar_aggregator.cpp, reused for consistency.
constexpr int64_t kBar1OpenUs = 1705359600LL * 1'000'000LL;       // 18:00:00 ET
constexpr int64_t kBar1MidUs = 1705360050LL * 1'000'000LL;        // 18:07:30 ET
constexpr int64_t kBar1LastTickUs = 1705360499LL * 1'000'000LL;   // 18:14:59 ET
constexpr int64_t kBar2OpenUs = 1705360500LL * 1'000'000LL;       // 18:15:00 ET (next TS3 bar)

}  // namespace

int main() {
    std::printf("MarketDataReplayEngine unit tests (Task 1: TickBarAggregator wiring)\n");

    // Three ticks within the same TS3 (15-min) bar, then a 4th tick at the next
    // TS3 boundary: TS3 should close exactly one bar; TS1 (240m)/TS2 (60m) stay
    // in their first bar the whole time (all 4 ticks fall within 18:00-19:00 ET).
    {
        MarketDataReplayEngine engine;
        engine.OnTick(kBar1OpenUs, 100.0, 10, 6, 4);
        engine.OnTick(kBar1MidUs, 105.0, 20, 12, 8);
        engine.OnTick(kBar1LastTickUs, 98.0, 5, 2, 3);
        check("no_ts3_bar_closed_yet_within_same_bucket", engine.GetTs3BarsClosed() == 0);
        check("ts1_no_bar_closed_yet", engine.GetTs1BarsClosed() == 0);
        check("ts2_no_bar_closed_yet", engine.GetTs2BarsClosed() == 0);

        engine.OnTick(kBar2OpenUs, 102.0, 7, 4, 3);
        check("ts3_bar_closed_exactly_at_next_boundary", engine.GetTs3BarsClosed() == 1);
        check("ts1_still_no_bar_closed", engine.GetTs1BarsClosed() == 0);
        check("ts2_still_no_bar_closed", engine.GetTs2BarsClosed() == 0);

        // Flush() closes the still-open bar on all 3 timeframes.
        engine.Flush();
        check("flush_closes_ts1_bar", engine.GetTs1BarsClosed() == 1);
        check("flush_closes_ts2_bar", engine.GetTs2BarsClosed() == 1);
        check("flush_closes_second_ts3_bar", engine.GetTs3BarsClosed() == 2);
    }

    // OnTick() always returns false in this task (no gate wired yet, Task 8).
    {
        MarketDataReplayEngine engine;
        bool significant = engine.OnTick(kBar1OpenUs, 100.0, 10, 6, 4);
        check("on_tick_returns_false_before_gate_is_wired", significant == false);
    }

    // GetObservation() is accessible and zero-initialized before any dim math
    // is wired (Task 2+).
    {
        MarketDataReplayEngine engine;
        const auto& obs = engine.GetObservation();
        check("observation_starts_zero_initialized", obs.log_scale_ratio() == 0.0f);
    }

    // Task 2: burstiness_index -- a uniformly-spaced tick stream should read
    // as "not bursty" (low magnitude); a stream with a long quiet run then a
    // rapid cluster should read as more bursty than the uniform one.
    {
        MarketDataReplayEngine uniform;
        int64_t t = kBar1OpenUs;
        for (int i = 0; i < 60; ++i) {
            uniform.OnTick(t, 100.0, 1, 1, 1);  // constant price -- no price-change gate firing
            t += 1'000'000LL;  // 1 second apart, every tick
        }
        const float uniformBurst = uniform.GetObservation().burstiness_index();

        MarketDataReplayEngine bursty;
        t = kBar1OpenUs;
        // A burst concentrated in only 1 of 10 bins can't move a median/MAD-based
        // statistic at all (by design -- the same >50%-breakdown robustness this
        // codebase's other median/MAD estimators rely on, Rousseeuw & Croux 1993).
        // This fixture instead packs 45 ticks into bin0 (1ms apart) then spreads 5
        // ticks one-per-remaining-bin (20s apart) so the burst is visible to the
        // robust statistic, not washed out by it.
        for (int i = 0; i < 45; ++i) {
            bursty.OnTick(t, 100.0, 1, 1, 1);
            t += 1'000LL;  // 1 millisecond apart -- all land in bin0
        }
        for (int i = 0; i < 5; ++i) {
            t += 20'000'000LL;  // 20 seconds apart -- one per remaining bin
            bursty.OnTick(t, 100.0, 1, 1, 1);
        }
        const float burstyBurst = bursty.GetObservation().burstiness_index();

        // Goh-Barabási bounded transform (EventVelocityEngine.h): -1.0 = perfectly
        // regular/uniform (the floor), +1.0 = maximally clustered. A perfectly
        // uniform 6-ticks-per-bin split has median=6, MAD=0 -> idc=0 -> exactly
        // -1.0, not "near zero" -- fixed this test's own wrong assumption, not
        // the engine.
        check("uniform_spacing_reads_at_the_regular_floor", uniformBurst < -0.9f);
        check("bursty_cluster_reads_more_bursty_than_uniform", burstyBurst > uniformBurst);
    }

    // Task 2: tail_index/lempel_ziv -- fed only on a genuine price change
    // (both share ContextManager::UpdateMarketPhysics()'s own gate); after
    // enough varying-price ticks both should move off their zero-initialized
    // defaults.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        double price = 100.0;
        bool sawSignificantReturn = false;
        for (int i = 0; i < 80; ++i) {
            const double delta = 0.01 * static_cast<double>((i * 37) % 23 - 11);
            if (delta != 0.0) sawSignificantReturn = true;
            price += delta;
            engine.OnTick(t, price, 1, 1, 1);
            t += 1'000'000LL;
        }
        check("price_actually_varied_in_this_fixture", sawSignificantReturn);
        // tail_index is non-candidate (dim-selection spec §3, 2026-09-09) --
        // its GetHillAlpha() computation is skipped entirely, so it stays at
        // ObservationData's zero-initialized default forever, by design.
        check("tail_index_stays_at_zero_default_non_candidate_dim",
              engine.GetObservation().tail_index() == 0.0f);
        check("lempel_ziv_moved_off_zero_default", engine.GetObservation().lempel_ziv() != 0.0f);
    }

    // Task 3: TS1-owned dims (log_scale_ratio, hurst_exponent, fisher_info).
    // Exact hand-computed BV/DFA-Hurst arithmetic is impractically tedious to
    // derive by hand, so this is a bounds/warm-up characterization test
    // (matches this plan's own precedent from Task 2), not exact-value
    // verification.
    {
        // Before 100 TS1 bars have closed: still at construction-time defaults.
        MarketDataReplayEngine notWarmedUp;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs1PeriodUs = 240LL * 60 * 1'000'000LL;
        for (int i = 0; i < 50; ++i) {
            notWarmedUp.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 7) - 3), 1, 1, 1);
            t += kTs1PeriodUs;
        }
        check("ts1_hurst_stays_at_neutral_default_before_warmup",
              notWarmedUp.GetObservation().hurst_exponent() == 0.5f);
        check("ts1_log_scale_ratio_stays_at_zero_default_before_warmup",
              notWarmedUp.GetObservation().log_scale_ratio() == 0.0f);

        // After 103 ticks (101 TS1 bars closed, >= kTs1CloseWindow=100 CLOSED +
        // 1 live, Task 6b): warmed up. 103 (not 102) so the final tick's own
        // live price isn't the fixture's periodic pattern's exact symmetric
        // midpoint (100.0) -- fisher_info's Fisher-transform formula is
        // exactly 0.0 at that midpoint by construction, which coincided with
        // fisher_info's own zero-init default and made the "moved off
        // default" check spuriously fail at 102 ticks specifically (a
        // fixture coincidence, not an engine bug -- same class of issue as
        // this file's other periodic-fixture lessons).
        MarketDataReplayEngine warmedUp;
        t = kBar1OpenUs;
        for (int i = 0; i < 103; ++i) {
            warmedUp.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 7) - 3), 1, 1, 1);
            t += kTs1PeriodUs;
        }
        const auto& obs = warmedUp.GetObservation();
        check("ts1_hurst_finite_and_bounded_after_warmup",
              std::isfinite(obs.hurst_exponent()) && obs.hurst_exponent() >= 0.0f &&
              obs.hurst_exponent() <= 1.0f);
        check("ts1_log_scale_ratio_finite_and_within_clamp_after_warmup",
              std::isfinite(obs.log_scale_ratio()) && std::fabs(obs.log_scale_ratio()) <= 6.0f);
        check("ts1_fisher_info_finite_after_warmup", std::isfinite(obs.fisher_info()));

        // Regression guard: the bounds checks above pass trivially even if
        // ComputeTs1Dims() never ran at all (defaults are finite/in-bounds too) --
        // this caught a real off-by-one bug this session where the warm-up guard
        // never actually passed. Assert real computation happened by checking the
        // values moved OFF their construction-time defaults.
        check("ts1_log_scale_ratio_moved_off_zero_default_after_warmup",
              obs.log_scale_ratio() != 0.0f);
        check("ts1_fisher_info_moved_off_zero_default_after_warmup",
              obs.fisher_info() != 0.0f);
        check("ts1_hurst_moved_off_neutral_default_after_warmup",
              obs.hurst_exponent() != 0.5f);
    }

    // Task 4: TS2-owned dims (relative_range, log_scale_expansion_ratio,
    // fractal_dim). Each warms up on its own timeline -- check both the
    // "still at default" and "moved off default" sides for each, per the
    // Task 3 lesson (bounds-only checks pass trivially on unwarmed defaults).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;

        // relative_range = (high-low)/ATR -- with single-tick bars, high==low
        // ALWAYS (a single trade print has no intrabar range), so the numerator
        // is exactly 0 regardless of how many bars close or what ATR is. This
        // is the SECOND wrong test-fixture assumption in a row for this dim
        // (the "gap" fixture above was also insufficient) -- fixed by giving
        // bar #1 genuine intrabar range via multiple ticks inside the same
        // TS2 bucket.
        //
        // Task 6b: relative_range is now computed from the LIVE (still-
        // forming) bar's own high/low every tick, not the just-closed bar --
        // so this checks the value WHILE bar #1 is still forming (after its
        // 3rd tick gives it real range), not after it closes into bar #2
        // (which would start a fresh single-point live bar with range 0).
        engine.OnTick(t, 100.0, 1, 1, 1);                    // bar #1 opens
        engine.OnTick(t + 1'800'000'000LL, 105.0, 1, 1, 1);  // +30min, still bar #1 (high=105)
        engine.OnTick(t + 3'000'000'000LL, 98.0, 1, 1, 1);   // +50min, still bar #1 (low=98)
        check("ts2_relative_range_moves_off_zero_with_real_intrabar_range",
              engine.GetObservation().relative_range() != 0.0f);
        t += kTs2PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // next bucket -> closes bar #1, opens bar #2
        t += kTs2PeriodUs;
        check("ts2_fractal_dim_stays_at_cold_start_default_before_warmup",
              engine.GetObservation().fractal_dim() == 1.5f);
        check("ts2_log_scale_expansion_ratio_stays_at_zero_default_before_warmup",
              engine.GetObservation().log_scale_expansion_ratio() == 0.0f);

        // 30 more bars: enough to warm up log_scale_expansion_ratio (needs 21
        // closes) but not yet fractal_dim (needs 401).
        for (int i = 0; i < 30; ++i) {
            engine.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 7) - 3), 1, 1, 1);
            t += kTs2PeriodUs;
        }
        check("ts2_log_scale_expansion_ratio_stays_at_zero_default_non_candidate_dim",
              engine.GetObservation().log_scale_expansion_ratio() == 0.0f);
        check("ts2_fractal_dim_still_at_cold_start_default",
              engine.GetObservation().fractal_dim() == 1.5f);

        // 400 more bars: enough to warm up fractal_dim too (needs 401 total closes).
        for (int i = 0; i < 400; ++i) {
            engine.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 7) - 3), 1, 1, 1);
            t += kTs2PeriodUs;
        }
        const auto& obs = engine.GetObservation();
        check("ts2_fractal_dim_moved_off_cold_start_default", obs.fractal_dim() != 1.5f);
        check("ts2_fractal_dim_within_valid_contract_range",
              obs.fractal_dim() >= 1.0f && obs.fractal_dim() <= 2.0f);
        check("ts2_log_scale_expansion_ratio_finite_and_within_clamp",
              std::isfinite(obs.log_scale_expansion_ratio()) &&
              obs.log_scale_expansion_ratio() >= -10.0f && obs.log_scale_expansion_ratio() <= 6.0f);
    }

    // Task 4b: bars_since_last_update (regime tenure) -- entirely physics-based
    // (Kaufman efficiency ratio + rolling volatility, 20-bar window); resets to
    // 1 on a >15% change in either, else increments.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;

        // Before the 20-bar window warms up: stays at its construction default.
        for (int i = 0; i < 15; ++i) {
            engine.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 5) - 2), 1, 1, 1);
            t += kTs2PeriodUs;
        }
        check("ts2_regime_tenure_stays_at_zero_default_before_warmup",
              engine.GetBarsSinceLastUpdate() == 0.0f);

        // Stable regime: small oscillating returns repeated -- tenure should
        // climb well past 1 without resetting.
        for (int i = 0; i < 40; ++i) {
            engine.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 5) - 2), 1, 1, 1);
            t += kTs2PeriodUs;
        }
        check("ts2_regime_tenure_climbs_well_past_one_in_a_stable_regime",
              engine.GetBarsSinceLastUpdate() > 5.0f);

        // Inject a genuine regime shift: a large, sustained jump changes both
        // the efficiency ratio and volatility far more than the 15% threshold.
        // Task 6b: regime tenure is now genuinely per-TICK reactive (both the
        // live efficiency/volatility reading AND the "previous" value it's
        // compared against update every tick, confirmed against
        // ContextManager::SetWaveContext's real per-tick commit) -- the reset
        // fires on this VERY FIRST shocked tick, not two ticks later.
        engine.OnTick(t, 250.0, 1, 1, 1);
        check("ts2_regime_tenure_resets_to_one_after_a_genuine_shock",
              engine.GetBarsSinceLastUpdate() == 1.0f);
    }

    // Task 5: TS3-owned dims (amihud_illiquidity, liq_fragility, mean_rev_z,
    // micro_asymmetry).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;  // 15 min

        // micro_asymmetry is non-candidate (dim-selection spec §3,
        // 2026-09-09) -- its computation is skipped entirely, so it stays at
        // ObservationData's zero-initialized default forever, by design.
        engine.OnTick(t, 100.0, 100, /*askVolume=*/80, /*bidVolume=*/20);
        check("ts3_micro_asymmetry_stays_at_zero_default_non_candidate_dim",
              engine.GetObservation().micro_asymmetry() == 0.0f);
        t += kTs3PeriodUs;

        // amihud_illiquidity/liq_fragility/mean_rev_z all stay at their
        // construction-time defaults before their respective windows warm up
        // (21/30/20 closed bars respectively; only a handful closed so far).
        for (int i = 0; i < 9; ++i) {
            engine.OnTick(t, 100.0 + 0.05 * static_cast<double>((i % 7) - 3), 1, 80, 20);
            t += kTs3PeriodUs;
        }
        check("ts3_amihud_stays_at_neutral_default_before_warmup",
              engine.GetObservation().amihud_illiquidity() == 0.5f);
        check("ts3_liq_fragility_stays_at_zero_default_before_warmup",
              engine.GetObservation().liq_fragility() == 0.0f);
        check("ts3_mean_rev_z_stays_at_zero_default_before_warmup",
              engine.GetObservation().mean_rev_z() == 0.0f);

        // Enough more bars to warm up all three (34 closed bars total >= the
        // largest requirement, liq_fragility's 30). Each historical bar gets a
        // SECOND tick with a different price to give it genuine intrabar
        // range -- liq_fragility's scale reference collapses to 0 (same
        // lesson as Task 4's relative_range) with single-tick bars.
        for (int i = 0; i < 33; ++i) {
            const double base = 100.0 + 0.05 * static_cast<double>((i % 7) - 3);
            engine.OnTick(t, base, 1, 80, 20);
            engine.OnTick(t + 300'000'000LL, base + 0.3, 1, 80, 20);  // +5min, real intrabar range
            t += kTs3PeriodUs;
        }
        // liq_fragility measures whether the CURRENT (still-forming) bar's
        // range-per-unit-volume deviates from the historical baseline -- a
        // uniform range/volume pattern across every bar (as above) has no
        // elasticity anomaly at all, so 0.0 is its genuinely correct neutral
        // output there, not a sign the test is warmed up incorrectly. The
        // live bar also needs cumulative volume >= its own kLiveBarMinVolume
        // (50) guard before it updates at all (below that, it carries
        // prev_fragility forward unchanged) -- give the final, still-forming
        // bar both a deliberately anomalous range AND enough volume to clear
        // that gate.
        engine.OnTick(t, 100.0, 30, 5, 1);
        engine.OnTick(t + 300'000'000LL, 103.0, 30, 5, 1);
        const auto& obs = engine.GetObservation();
        check("ts3_amihud_moved_off_neutral_default_after_warmup", obs.amihud_illiquidity() != 0.5f);
        check("ts3_liq_fragility_moved_off_zero_default_after_warmup", obs.liq_fragility() != 0.0f);
        check("ts3_liq_fragility_within_valid_contract_range",
              obs.liq_fragility() >= 0.0f && obs.liq_fragility() <= 1.0f);
        check("ts3_mean_rev_z_moved_off_zero_default_after_warmup", obs.mean_rev_z() != 0.0f);
        check("ts3_mean_rev_z_within_valid_contract_range",
              obs.mean_rev_z() >= 0.0f && obs.mean_rev_z() <= 5.0f);
    }

    // Task 6: Activity-clock dims (skewness_idx, fast_taleb_kurtosis,
    // fast_hurst_exponent, recurrence_rate) -- own ImbalanceBarEngine,
    // independent of the TS1/TS2/TS3 TickBarAggregators above.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTickPeriodUs = 1'000'000LL;  // cadence irrelevant to ImbalanceBarEngine

        check("activity_clock_fast_taleb_kurtosis_stays_at_neutral_default_before_warmup",
              engine.GetObservation().fast_taleb_kurtosis() == 1.23f);
        check("activity_clock_skewness_idx_stays_at_zero_default_before_warmup",
              engine.GetObservation().skewness_idx() == 0.0f);
        check("activity_clock_fast_hurst_exponent_stays_at_neutral_default_before_warmup",
              engine.GetObservation().fast_hurst_exponent() == 0.5f);
        check("activity_clock_recurrence_rate_stays_at_zero_default_before_warmup",
              engine.GetObservation().recurrence_rate() == 0.0f);

        // 500 ticks: askVolume=10/bidVolume=0 each (net +10 imbalance/tick)
        // closes an imbalance bar every 5th tick (default threshold=50) -> 100
        // completed bars, each spanning 5 ticks of real price movement (not a
        // degenerate single-tick bar, which would give
        // barReturn=log(price/price)=0 always since bar-open==bar-close price
        // when a bar completes on its own opening tick). A simple LCG-driven
        // price walk, not a periodic formula: an earlier attempt using
        // `100 + 0.001*i + 0.02*((i%7)-3)` created bar returns quasi-periodic
        // against the imbalance engine's own 5-tick bar cadence (5 vs 7 share
        // no common structure, but the pattern was still fully deterministic/
        // regular) -- BowleySkewness/MoorsKurtosis are sensitive to that kind
        // of clumping in a small sample and produced skewness_idx=0.998 (right
        // at its [-1,1] mathematical bound) and fast_taleb_kurtosis=0.0025
        // (outside the [0.5,8.0] contract), same class of "too-regular-a-
        // fixture" issue as Task 5's earlier liq_fragility debugging.
        uint32_t seed = 12345;
        double price = 100.0;
        for (int i = 0; i < 500; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;  // [-1,1]
            price += 0.05 * r;
            engine.OnTick(t, price, 1, 10, 0);
            t += kTickPeriodUs;
        }
        const auto& obs2 = engine.GetObservation();
        // All 4 activity-clock dims are non-candidate (dim-selection spec §3,
        // 2026-09-09) -- ComputeActivityClockDims()'s downstream calculators
        // (MoorsKurtosis/BowleySkewness/DfaHurstExponent/RQA rebuild) are
        // skipped entirely, so all 4 stay at their construction-time defaults
        // forever, by design.
        check("activity_clock_fast_taleb_kurtosis_stays_at_neutral_default_non_candidate_dim",
              obs2.fast_taleb_kurtosis() == 1.23f);
        check("activity_clock_fast_taleb_kurtosis_within_valid_contract_range",
              obs2.fast_taleb_kurtosis() >= 0.5f && obs2.fast_taleb_kurtosis() <= 8.0f);
        check("activity_clock_skewness_idx_stays_at_zero_default_non_candidate_dim",
              obs2.skewness_idx() == 0.0f);
        check("activity_clock_skewness_idx_within_valid_contract_range",
              obs2.skewness_idx() >= -2.5f && obs2.skewness_idx() <= 2.5f);
        check("activity_clock_fast_hurst_exponent_stays_at_neutral_default_non_candidate_dim",
              obs2.fast_hurst_exponent() == 0.5f);
        check("activity_clock_fast_hurst_exponent_within_valid_contract_range",
              obs2.fast_hurst_exponent() >= 0.0f && obs2.fast_hurst_exponent() <= 1.5f);
        check("activity_clock_recurrence_rate_stays_at_zero_default_non_candidate_dim",
              obs2.recurrence_rate() == 0.0f);
        check("activity_clock_recurrence_rate_within_valid_contract_range",
              obs2.recurrence_rate() >= 0.0f && obs2.recurrence_rate() <= 1.0f);
    }

    // Task 6b: intra-bar reactivity remediation. Production recomputes
    // hurst_exponent/fisher_info/log_scale_ratio (TS1), relative_range/
    // fractal_dim/log_scale_expansion_ratio/bars_since_last_update (TS2), and
    // mean_rev_z (TS3) EVERY TICK from a window ending at the current,
    // still-forming bar -- no bar-close gate exists in production for any of
    // these 8 dims (spec §2a). A single extra tick landing strictly INSIDE
    // the currently-forming bar (no TS1/TS2/TS3 boundary crossed) must change
    // every one of them once warmed up; a bar-gated implementation leaves all
    // 8 frozen at their last bar-close value.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;
        uint32_t seed = 777;
        int64_t lastBucketStart = t;

        // 410 TS2-periods (2 ticks each, real intrabar range) warms up TS1
        // (needs 101 closes @ 240min = ~404 TS2-periods) and TS2 (needs 401
        // closes @ 60min) simultaneously.
        for (int i = 0; i < 410; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double base = 100.0 + 0.05 * (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            lastBucketStart = t;
            engine.OnTick(t, base, 1, 1, 1);
            engine.OnTick(t + 1'800'000'000LL, base + 0.3, 1, 1, 1);  // +30min
            t += kTs2PeriodUs;
        }
        // 30-bar STABLE tail: regime tenure (unlike the other 7 dims here) is
        // genuinely per-TICK reactive on BOTH sides of its own comparison (the
        // "previous" value it compares against also updates every tick,
        // confirmed against ContextManager::SetWaveContext's real per-tick
        // commit) -- the noisy LCG warm-up above keeps resetting it to 1 on
        // nearly every tick, so "before" and "after" the mid-bar jump below
        // would both land on the same floor value (1==1), a false negative
        // for "reacts mid-bar" even though the mechanism IS reacting every
        // tick. A calm tail lets tenure climb above 1 first, so the reset
        // down to 1 from the price jump is genuinely detectable.
        for (int i = 0; i < 30; ++i) {
            const double base = 100.0 + 0.02 * static_cast<double>((i % 7) - 3);
            lastBucketStart = t;
            engine.OnTick(t, base, 1, 1, 1);
            engine.OnTick(t + 1'800'000'000LL, base, 1, 1, 1);
            t += kTs2PeriodUs;
        }
        const auto& obsBefore = engine.GetObservation();
        const float hurstBefore = obsBefore.hurst_exponent();
        const float fisherBefore = obsBefore.fisher_info();
        const float logScaleRatioBefore = obsBefore.log_scale_ratio();
        const float relRangeBefore = obsBefore.relative_range();
        const float fractalDimBefore = obsBefore.fractal_dim();
        const float logScaleExpBefore = obsBefore.log_scale_expansion_ratio();
        const float tenureBefore = engine.GetBarsSinceLastUpdate();
        const float meanRevZBefore = obsBefore.mean_rev_z();

        // +35min into the last bucket: still inside the same 60-min TS2/
        // 240-min TS1/15-min TS3 bucket as the loop's own last 2 ticks (30min
        // apart) -- no boundary crossed anywhere, confirmed via
        // GetTsXBarsClosed() staying unchanged below.
        const int ts1ClosedBefore = engine.GetTs1BarsClosed();
        const int ts2ClosedBefore = engine.GetTs2BarsClosed();
        const int ts3ClosedBefore = engine.GetTs3BarsClosed();
        engine.OnTick(lastBucketStart + 2'100'000'000LL, 150.0, 1, 1, 1);
        check("task6b_no_boundary_crossed_by_the_mid_bar_tick",
              engine.GetTs1BarsClosed() == ts1ClosedBefore &&
              engine.GetTs2BarsClosed() == ts2ClosedBefore &&
              engine.GetTs3BarsClosed() == ts3ClosedBefore);

        const auto& obsAfter = engine.GetObservation();
        check("task6b_hurst_exponent_reacts_mid_bar", obsAfter.hurst_exponent() != hurstBefore);
        check("task6b_fisher_info_reacts_mid_bar", obsAfter.fisher_info() != fisherBefore);
        check("task6b_log_scale_ratio_reacts_mid_bar", obsAfter.log_scale_ratio() != logScaleRatioBefore);
        check("task6b_relative_range_reacts_mid_bar", obsAfter.relative_range() != relRangeBefore);
        check("task6b_fractal_dim_reacts_mid_bar", obsAfter.fractal_dim() != fractalDimBefore);
        // log_scale_expansion_ratio is non-candidate (dim-selection spec §3,
        // 2026-09-09) -- its computation is skipped entirely, so it no longer
        // reacts mid-bar (or at all); asserts it stays exactly unchanged.
        check("task6b_log_scale_expansion_ratio_stays_unchanged_non_candidate_dim",
              obsAfter.log_scale_expansion_ratio() == logScaleExpBefore);
        check("task6b_bars_since_last_update_reacts_mid_bar",
              engine.GetBarsSinceLastUpdate() != tenureBefore);
        check("task6b_mean_rev_z_reacts_mid_bar", obsAfter.mean_rev_z() != meanRevZBefore);

        // Task 7: fast_mean_rev_z is a decided-DROP dim (spec §2) -- never
        // wired in the existing codebase (ActivityClockMeanReversion.h is
        // real and pure but genuinely unused in the live path), so this
        // engine must never assign it a real value either. A guard, not a
        // feature: reuses this block's own fully-warmed-up engine (every
        // other dim has been exercised above) as the strongest test of
        // "nothing accidentally wires this field".
        check("task7_fast_mean_rev_z_stays_zero_sentinel_after_full_warmup",
              obsAfter.fast_mean_rev_z() == 0.0f);
    }

    // Task 8: the real collection-mode gate (quality-over-quantity
    // correction, 2026-09-08) -- FeatureScaler's own 500-sample warmup gate
    // + the Mahalanobis ObservationTriggerGate, the SAME gate now shared by
    // both live-trading and data-collection paths in ContextManager.cpp
    // (ShouldTriggerHMM() collapsed to one branch). Reuses the Task 6b
    // warm-up pattern (hour-scale ticks, 2 per period) since FeatureScaler's
    // scaling needs genuinely-varying dims across the whole vector, not just
    // tick-level ones -- a second-scale fixture leaves most TS1/TS2/TS3 dims
    // frozen at cold-start defaults for the whole run (not enough elapsed
    // session time to close any bars), verified via a standalone debug
    // harness before committing to this fixture.
    //
    // Task 10 correction: AllDimsReady() (added after this test was first
    // written) gates ComputeShouldEmit()'s very first line, so FeatureScaler's
    // OWN 500-sample counter does not even START incrementing until TS1's
    // 100-bar requirement is satisfied (~400 iterations at 1 hour/iteration)
    // -- the two warm-ups are now SEQUENTIAL, not concurrent. 440 iterations
    // was long enough before Task 10 but left almost no runway afterward
    // (found via a standalone debug harness: every single tick returned
    // false, not just the ones during warmup, since FeatureScaler itself
    // never finished warming up within the fixture's remaining length).
    // Extended to 750 iterations and the warmup/quiet split moved from a
    // fixed tick count to the TS1-warmup boundary itself (iteration 400).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;
        uint32_t seed = 777;
        int trueDuringWarmup = 0;
        int trueDuringQuiet = 0;
        int quietTickCount = 0;
        for (int i = 0; i < 750; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double base = 100.0 + 0.05 * (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            const bool r1 = engine.OnTick(t, base, 1, 60, 0);
            const bool r2 = engine.OnTick(t + 1'800'000'000LL, base + 0.3, 1, 60, 0);
            // AllDimsReady()'s slowest requirement (TS1's 100-bar/240-min
            // window) can't possibly be satisfied before iteration 400 (100
            // bars * 4h/bar = 400h = 400 iterations at 1h/iteration) --
            // every tick strictly before that MUST be false.
            if (i < 400) {
                if (r1) ++trueDuringWarmup;
                if (r2) ++trueDuringWarmup;
            } else {
                if (r1) ++trueDuringQuiet;
                ++quietTickCount;
                if (r2) ++trueDuringQuiet;
                ++quietTickCount;
            }
            t += kTs2PeriodUs;
        }
        check("task8_no_premature_trigger_before_ts1_warmup_boundary",
              trueDuringWarmup == 0);
        // The old any-dim-moved-by-epsilon gate would fire on nearly every
        // tick (spec's own finding); the real Mahalanobis gate must fire on
        // meaningfully FEWER than half the ticks even on this noisy
        // synthetic walk.
        check("task8_quiet_period_does_not_trigger_on_nearly_every_tick",
              trueDuringQuiet > 0 && trueDuringQuiet < quietTickCount / 2);

        // A genuine, large regime shift must still trigger reliably.
        const bool jump = engine.OnTick(t, 500.0, 1, 60, 0);
        check("task8_genuine_regime_shift_triggers_emission", jump);
    }

    // Task 10: warm-up gating (spec §3a open Q2). FeatureScaler's own 500-
    // sample warmup gate (Task 8) counts TICKS, not elapsed session time --
    // on a short-elapsed-time run (many ticks within the first few hours of
    // the first session), FeatureScaler can warm up on tick count alone
    // while TS1/TS2's bar-based dims (needing ~16+ days of elapsed time for
    // TS1's 100 240-minute bars) are still frozen at cold-start defaults.
    // Without this gate, `.context` would emit placeholder-masquerading-as-
    // real values -- confirmed via a standalone debug harness before
    // implementing (26 emissions over 700 one-second ticks, none of them
    // genuine). No dim's real, computed value should ever be emitted until
    // EVERY dim's own warm-up requirement is satisfied, not just
    // FeatureScaler's.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        uint32_t seed = 99;
        double price = 100.0;
        int trueCount = 0;
        for (int i = 0; i < 700; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.05 * r;
            if (engine.OnTick(t, price, 1, 60, 0)) ++trueCount;
            t += 1'000'000LL;
        }
        check("task10_no_emission_before_ts1_ts2_bar_based_dims_warm_up", trueCount == 0);
    }

    // Task 11: edge-case hardening (spec §3f).
    {
        // (a) zero/negative price is skipped entirely, not fed to any engine
        // -- confirmed via TS1/TS2/TS3 bar-close counts staying unchanged
        // (a fed degenerate tick would still count as opening/advancing a
        // bucket even with a garbage price).
        MarketDataReplayEngine engine;
        engine.OnTick(kBar1OpenUs, 100.0, 1, 1, 1);
        const int ts3BarsBefore = engine.GetTs3BarsClosed();
        const bool sigZero = engine.OnTick(kBar1OpenUs + 1'000'000LL, 0.0, 1, 1, 1);
        const bool sigNeg = engine.OnTick(kBar1OpenUs + 2'000'000LL, -5.0, 1, 1, 1);
        check("task11_zero_price_tick_returns_false_and_is_not_fed_to_engines",
              !sigZero && !sigNeg && engine.GetTs3BarsClosed() == ts3BarsBefore);
    }
    {
        // (b) a timestamp that goes backwards is a hard error (throw), not a
        // silent reorder -- the input is assumed already-resequenced
        // upstream (scid_to_ticks_parquet.cpp), so ANY backwards jump here
        // signals real data corruption.
        MarketDataReplayEngine engine;
        engine.OnTick(kBar1OpenUs, 100.0, 1, 1, 1);
        bool threw = false;
        try {
            engine.OnTick(kBar1OpenUs - 1'000'000LL, 100.0, 1, 1, 1);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("task11_backwards_timestamp_throws", threw);
    }
    {
        // (c) NaN from a degenerate window carries forward the last valid
        // value into m_obs, never propagates a NaN into FeatureScaler --
        // regression guard for a pattern already used throughout (e.g.
        // ComputeActivityClockDims's `if (std::isfinite(x)) ...` sites).
        // Forces a genuinely degenerate skewness/kurtosis window (a run of
        // IDENTICAL imbalance-bar returns collapses BowleySkewness/
        // MoorsKurtosis's quartile-spread denominators to exactly zero, per
        // RobustMoments.h's own documented NaN contract) after first
        // warming up normally so there's a real "last valid" value to carry
        // forward from.
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        uint32_t seed = 555;
        double price = 100.0;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.05 * r;
            engine.OnTick(t, price, 1, 60, 0);
            t += 1'000'000LL;
        }
        const float skewnessBefore = engine.GetObservation().skewness_idx();
        const float kurtosisBefore = engine.GetObservation().fast_taleb_kurtosis();
        // Identical price every tick -> every imbalance-bar return is
        // exactly 0.0 -> degenerate (zero-spread) quantile window.
        for (int i = 0; i < 200; ++i) {
            engine.OnTick(t, price, 1, 60, 0);
            t += 1'000'000LL;
        }
        const auto& obs = engine.GetObservation();
        check("task11_nan_skewness_carries_forward_last_valid_value",
              obs.skewness_idx() == skewnessBefore && std::isfinite(obs.skewness_idx()));
        check("task11_nan_kurtosis_carries_forward_last_valid_value",
              obs.fast_taleb_kurtosis() == kurtosisBefore && std::isfinite(obs.fast_taleb_kurtosis()));
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
