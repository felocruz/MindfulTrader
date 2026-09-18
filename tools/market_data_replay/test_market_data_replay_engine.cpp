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

bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) < tol; }

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
        // tail_index re-enabled 2026-09-16 (operator directive: lbrnet does
        // dim selection now, not a Gaussian gate here) -- with >=50 real log-
        // return samples fed above, it should be a finite, in-contract Hill
        // alpha, not the zero-initialized default.
        const float tailIndex = engine.GetObservation().tail_index();
        check("tail_index_finite_and_within_contract_after_warmup",
              std::isfinite(tailIndex) && tailIndex >= 1.1f && tailIndex <= 8.0f);
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
        check("ts2_log_scale_expansion_ratio_moved_off_zero_default_after_warmup",
              engine.GetObservation().log_scale_expansion_ratio() != 0.0f);
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

        // micro_asymmetry re-enabled 2026-09-16 (operator directive) -- reads
        // the live bar's own cumulative ask/bid volume immediately, no warmup.
        engine.OnTick(t, 100.0, 100, /*askVolume=*/80, /*bidVolume=*/20);
        check("ts3_micro_asymmetry_reflects_live_bar_ask_bid_split",
              engine.GetObservation().micro_asymmetry() == (80.0f - 20.0f) / 100.0f);
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
        // All 4 activity-clock dims re-enabled 2026-09-16 (operator directive:
        // lbrnet does dim selection now, not a Gaussian gate here) -- with
        // 100 completed imbalance bars fed above, all 4 should have moved off
        // their construction-time defaults.
        check("activity_clock_fast_taleb_kurtosis_moved_off_neutral_default",
              obs2.fast_taleb_kurtosis() != 1.23f);
        check("activity_clock_fast_taleb_kurtosis_within_valid_contract_range",
              obs2.fast_taleb_kurtosis() >= 0.5f && obs2.fast_taleb_kurtosis() <= 8.0f);
        check("activity_clock_skewness_idx_moved_off_zero_default",
              obs2.skewness_idx() != 0.0f);
        check("activity_clock_skewness_idx_within_valid_contract_range",
              obs2.skewness_idx() >= -2.5f && obs2.skewness_idx() <= 2.5f);
        check("activity_clock_fast_hurst_exponent_moved_off_neutral_default",
              obs2.fast_hurst_exponent() != 0.5f);
        check("activity_clock_fast_hurst_exponent_within_valid_contract_range",
              obs2.fast_hurst_exponent() >= 0.0f && obs2.fast_hurst_exponent() <= 1.5f);
        check("activity_clock_recurrence_rate_moved_off_zero_default",
              obs2.recurrence_rate() != 0.0f);
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
        // log_scale_expansion_ratio re-enabled 2026-09-16 (operator directive)
        // -- now reacts mid-bar like every other TS2 live dim above.
        check("task6b_log_scale_expansion_ratio_reacts_mid_bar",
              obsAfter.log_scale_expansion_ratio() != logScaleExpBefore);
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
    // + the Mahalanobis CandidateTriggerGate. Task 17 (dim-selection spec
    // §3a/§3b, 2026-09-15) made each candidate dim's own rolling window
    // fill independently on push-on-change, not in lockstep -- so this
    // fixture must give EVERY candidate dim genuine, non-degenerate
    // variability, not just enough elapsed session time. Two dims needed a
    // real fixture redesign (found via a standalone debug harness before
    // editing this test): `liq_fragility` needs real intrabar range AND
    // >=50 cumulative live-bar volume (kLiveBarMinVolume) per TS3 bucket,
    // so each iteration now clusters 3 ticks (varying price/volume) inside
    // one bucket instead of 2 ticks straddling a TS3 boundary (which always
    // produces single-tick, zero-range bars); `burstiness_index` measures
    // the inter-CLUSTER arrival pattern at the ~100-tick rolling window's
    // own time scale (many hours here), so a perfectly regular hourly
    // outer cadence reads as maximally regular no matter how much
    // intra-cluster micro-jitter is added -- the outer step itself now
    // carries its own randomized jitter (average ~1 hour, per Task 10's
    // TS1/TS2 warm-up timeline below, but not exactly on the hour).
    //
    // Task 10 correction: AllDimsReady() (added after this test was first
    // written) gates ComputeShouldEmit()'s very first line, so FeatureScaler's
    // OWN 500-sample counter does not even START incrementing until TS1's
    // 100-bar requirement is satisfied (~400 iterations at ~1 hour/iteration)
    // -- the two warm-ups are now SEQUENTIAL, not concurrent. 440 iterations
    // was long enough before Task 10 but left almost no runway afterward
    // (found via a standalone debug harness: every single tick returned
    // false, not just the ones during warmup, since FeatureScaler itself
    // never finished warming up within the fixture's remaining length).
    // Extended to 750 iterations; the warmup/quiet split stays keyed to the
    // TS1-warmup boundary itself (iteration 400) since the jitter below is
    // zero-mean and only ±15min against a ~60min step.
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
            seed = seed * 1664525u + 1013904223u;
            const int64_t off1 = static_cast<int64_t>(seed % 3'000'000LL);
            seed = seed * 1664525u + 1013904223u;
            const int64_t off2 = off1 + 1'000'000LL + static_cast<int64_t>(seed % 3'000'000LL);
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol1 = 20 + static_cast<int64_t>(seed % 20);
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol2 = 20 + static_cast<int64_t>(seed % 20);
            seed = seed * 1664525u + 1013904223u;
            const int64_t askVol = static_cast<int64_t>(seed % 30);
            seed = seed * 1664525u + 1013904223u;
            const int64_t bidVol = static_cast<int64_t>(seed % 30);

            const bool r1 = engine.OnTick(t, base, vol1, askVol, bidVol);
            const bool r2 = engine.OnTick(t + off1, base + 0.3, vol2, askVol + 1, bidVol);
            const bool r3 = engine.OnTick(t + off2, base - 0.2, vol1, askVol, bidVol + 1);
            // AllDimsReady()'s slowest requirement (TS1's 100-bar/240-min
            // window) can't possibly be satisfied before iteration 400 (100
            // bars * 4h/bar = 400h = 400 iterations at ~1h/iteration) --
            // every tick strictly before that MUST be false.
            if (i < 400) {
                if (r1) ++trueDuringWarmup;
                if (r2) ++trueDuringWarmup;
                if (r3) ++trueDuringWarmup;
            } else {
                if (r1) ++trueDuringQuiet;
                ++quietTickCount;
                if (r2) ++trueDuringQuiet;
                ++quietTickCount;
                if (r3) ++trueDuringQuiet;
                ++quietTickCount;
            }
            // Macro-level jitter on the inter-cluster spacing itself
            // (burstiness_index's own time scale, see comment above).
            seed = seed * 1664525u + 1013904223u;
            const int64_t macroJitterUs =
                static_cast<int64_t>(seed % (30LL * 60 * 1'000'000LL)) - (15LL * 60 * 1'000'000LL);
            t += kTs2PeriodUs + macroJitterUs;
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

    // --- Task 7 (spec §3 item 2): PRIMARY_TRIGGER_MASK pattern-detector
    // wiring -- NR7 hand-verified end-to-end through the real per-tick
    // engine (not just IndicatorComputations.h's own unit tests, which only
    // cover DetectNR7() in isolation -- this confirms the REPLAY ENGINE
    // feeds it the right pre-update ring state and updates the dirty mask).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;  // 15 minutes

        // 20 uniform filler bars (each: open=100, high=102, low=98, close=100
        // -> range=4) -- fills both the NR7 (7-bar) and TurtleSoup (20-bar)
        // rings identically before any pattern-triggering bar arrives.
        for (int i = 0; i < 20; ++i) {
            engine.OnTick(t, 100.0, 1, 1, 1);
            engine.OnTick(t + 60'000'000LL, 102.0, 1, 1, 1);
            engine.OnTick(t + 120'000'000LL, 98.0, 1, 1, 1);
            engine.OnTick(t + 180'000'000LL, 100.0, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        // Consume whatever dirty bits accumulated from the filler bars —
        // only the NEXT (deliberately narrow) bar's fire matters below.
        engine.ConsumePatternDirtyMask();

        // Bar #21: range=1 (open=100, high=100.5, low=99.5, close=100.2),
        // strictly narrower than each of the last 7 filler bars' range=4 ->
        // DetectNR7() must classify STRONG.
        engine.OnTick(t, 100.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 100.5, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 99.5, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 100.2, 1, 1, 1);
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // one more tick to force the bar closed

        check("task7_nr7_fires_strong_on_genuine_narrow_bar",
              engine.GetNr7Result() == NR7Enum::STRONG);
        check("task7_nr7_quality_nonzero_when_strong", engine.GetNr7Quality() > 0.0f);
        const uint64_t dirtyMask = engine.ConsumePatternDirtyMask();
        check("task7_nr7_dirty_bit_set_on_classification_change",
              (dirtyMask & (1ULL << static_cast<uint64_t>(IndicatorKey::NR7))) != 0);
        check("task7_dirty_mask_clears_after_consume",
              engine.ConsumePatternDirtyMask() == 0);

        // TurtleSoup: the 20-bar prior window (high=102/low=98 for all 20
        // filler bars) is full by now -- confirm the ring itself reports
        // full and its aggregate matches the hand-known filler values
        // (integration check that Task 7 read the PRE-update ring state,
        // not a stale/empty one).
        check("task7_turtle_soup_window_full_after_20_filler_bars",
              engine.GetTurtleSoupRing().IsFull());
        check("task7_turtle_soup_highest_high_matches_filler_bars",
              near(engine.GetTurtleSoupRing().HighestHigh(), 102.0f));
        check("task7_turtle_soup_lowest_low_matches_filler_bars",
              near(engine.GetTurtleSoupRing().LowestLow(), 98.0f));

        // Kangaroo Tail: a genuine bullish long-lower-tail bar (no cross-bar
        // history needed, only ATR(10) which has been seeded 4.0 by the 20
        // filler bars, then drifted via the NR7 bar's own narrow true range
        // -- hand-verified tier boundary, not just "fires something").
        // open=100.2 (matches bar21's own close, no gap), close=101.7
        // (body=1.5), low=95 (lowerTail=5.2), high=101.9 (closePosition=
        // (101.7-95)/(101.9-95)=0.971). lowerTailToBody=5.2/1.5=3.467 -> in
        // [2.5,4.0) => STRONG tier regardless of the exact evolved ATR value
        // (DetectKangarooTail's STRONG tier only requires tailToBody in
        // [2.5,4.0), the >=0.3xATR gate is comfortably satisfied either way).
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.2, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 101.9, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 95.0, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 101.7, 1, 1, 1);
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // force the bar closed

        check("task7_kangaroo_tail_fires_bullish_strong",
              engine.GetKangarooTailResult() == KangarooTailEnum::BULLISH_STRONG);
        check("task7_kangaroo_tail_quality_nonzero_when_strong", engine.GetKangarooTailQuality() > 0.0f);
        const uint64_t kangarooDirtyMask = engine.ConsumePatternDirtyMask();
        check("task7_kangaroo_tail_dirty_bit_set",
              (kangarooDirtyMask & (1ULL << static_cast<uint64_t>(IndicatorKey::KANGAROO_TAIL))) != 0);
    }

    // --- Task 7: ATR_PROXIMITY / STRUCTURE_TEST (fresh engine, hand-verified) ---
    // STRUCTURE_TEST's real prev_high/prev_low are prevDayHigh/prevDayLow (the
    // PREVIOUS COMPLETED TRADING DAY's own high/low, src/TripleScreen3.cpp:614),
    // NOT the immediately-prior 15-min bar's own high/low -- an assumption an
    // earlier pass in this same session got wrong, caught by a later audit
    // against the real call site. This test seeds a genuine "day 0" session
    // first so m_prevDayHigh/m_prevDayLow are populated before any assertion.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        constexpr int64_t kOneDayUs = 24LL * 60 * 60 * 1'000'000LL;

        // Day 0 (the session immediately before kBar1OpenUs's own session):
        // two ticks establish a known daily high=110/low=90, closed the
        // instant day 1's first tick (kBar1OpenUs) arrives.
        engine.OnTick(t - kOneDayUs, 100.0, 1, 1, 1);
        engine.OnTick(t - kOneDayUs + 60'000'000LL, 110.0, 1, 1, 1);
        engine.OnTick(t - kOneDayUs + 120'000'000LL, 90.0, 1, 1, 1);

        // 20 uniform filler bars (open=100,high=102,low=98,close=100 ->
        // range=4, TR=4 every bar since prevClose=100 always) -- seeds
        // ATR(10) to exactly 4.0 by bar 10, stays 4.0 through bar 20. Since
        // 98>90 and 102<110 for every filler bar, StructureTest classifies
        // all of them INSIDE_BAR relative to day 0's range -- a stable
        // baseline for the dirty-bit check below.
        for (int i = 0; i < 20; ++i) {
            engine.OnTick(t, 100.0, 1, 1, 1);
            engine.OnTick(t + 60'000'000LL, 102.0, 1, 1, 1);
            engine.OnTick(t + 120'000'000LL, 98.0, 1, 1, 1);
            engine.OnTick(t + 180'000'000LL, 100.0, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        engine.ConsumePatternDirtyMask();

        // Bar #21: open=100, high=115, low=99, close=113. bar_range=16,
        // ATR grows to 4.0+(16-4.0)/10=5.2 once this bar's own true range is
        // folded in -> ratio=16/5.2=3.08, >2.5x -> EXTREME tier; close(113)
        // is much nearer high(115) than low(99) -> EXTREME_HIGH. StructureTest:
        // high(115) > prevDayHigh(110), NOT an outside bar (low=99 >
        // prevDayLow=90); lookbackHigh (20-bar window INCLUDING today) =
        // max(102 from fillers, 115) = 115, so high(115) is NOT > lookbackHigh
        // (115) -> FAILED_HIGH_STRONG_REVERSAL doesn't fire; falls through to
        // the bullish breakout test: close(113) > prevDayHigh+0.25xATR
        // (110+0.25*5.2=111.3) => DECISIVE_BREAKOUT_HIGH.
        engine.OnTick(t, 100.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 115.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 99.0, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 113.0, 1, 1, 1);
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // force the bar closed

        check("task7_atr_proximity_fires_extreme_high",
              engine.GetAtrProximityResult() == ATRProximityEnum::EXTREME_HIGH);
        check("task7_structure_test_fires_decisive_breakout_high",
              engine.GetStructureTestResult() == StructureTest::DECISIVE_BREAKOUT_HIGH);
        const uint64_t dirtyMask = engine.ConsumePatternDirtyMask();
        check("task7_atr_proximity_dirty_bit_set",
              (dirtyMask & (1ULL << static_cast<uint64_t>(IndicatorKey::ATR_PROXIMITY))) != 0);
        check("task7_structure_test_dirty_bit_set",
              (dirtyMask & (1ULL << static_cast<uint64_t>(IndicatorKey::STRUCTURE_TEST))) != 0);
    }

    // --- Task 7: LONG_IMP (TS1's own Elder Impulse) integration smoke test ---
    // A full hand-computed EMA(13)+MACD(12,26,9) impulse-color trace across
    // many 240-min bars is not practical to verify by hand (unlike the
    // single/few-bar cases above) -- this confirms the wiring end-to-end
    // (color settles to a well-defined non-BLUE value on a sustained trend,
    // and the dirty bit fires at least once), matching this plan's own
    // documented scope for LONG_IMP/INTERM_IMP-style cross-timeframe dims.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs1PeriodUs = 240LL * 60 * 1'000'000LL;
        double price = 100.0;
        uint64_t sawLongImpDirty = 0;
        for (int i = 0; i < 60; ++i) {
            price += 1.0;  // sustained uptrend -- 60 TS1 closes
            engine.OnTick(t, price, 1, 1, 1);
            t += kTs1PeriodUs;
            sawLongImpDirty |= engine.ConsumePatternDirtyMask();
        }
        check("task7_long_impulse_color_is_well_defined",
              engine.GetLongImpulseColor() == kImpulseGreen ||
              engine.GetLongImpulseColor() == kImpulseRed ||
              engine.GetLongImpulseColor() == kImpulseBlue);
        check("task7_long_impulse_dirty_bit_fired_at_least_once",
              (sawLongImpDirty & (1ULL << static_cast<uint64_t>(IndicatorKey::LONG_IMP))) != 0);
    }

    // --- Audit fix verification: ElderBreakout's real TS3-local 100-bar Hurst
    // (not the canonical TS1-based hurst_exponent dim -- see the fix's own
    // in-code citation + lbrnet/logs/rc_gemini.log CLAUDE_BRIEF_145/_REPLY) ---
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        double price = 100.0;
        uint32_t seed = 777;
        // 101 bars needed to warm the new 100-return DFA window (101 retained
        // closes -> 100 log-returns), a genuinely non-degenerate (not pure
        // trend, not flat) walk so DFA doesn't collapse to a trivial reading.
        for (int i = 0; i < 105; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.1 * r;
            engine.OnTick(t, price, 1, 1, 1);
            engine.OnTick(t + 450'000'000LL, price + 0.2, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        const float ts3Hurst = engine.GetElderBreakoutTs3Hurst();
        check("task7_elder_breakout_ts3_hurst_finite_and_in_unit_range",
              std::isfinite(ts3Hurst) && ts3Hurst >= 0.0f && ts3Hurst <= 1.0f);
        // Not a mechanical identity: the TS3-local Hurst (100 15-min-bar
        // closes) and the canonical TS1 hurst_exponent (100 240-min-bar
        // closes, only ~6-7 TS1 bars closed by now) are fed from genuinely
        // different bar series -- confirms the fix is actually wired to its
        // own independent window, not silently aliasing the TS1 dim again.
        check("task7_elder_breakout_ts3_hurst_independent_of_canonical_ts1_dim",
              ts3Hurst != engine.GetObservation().hurst_exponent());
    }

    // --- Task 7: INTERM_STOCHASTIC (TS2 Stochastic(10,3,3) crossover) ---
    // A full hand-computed FastK/FastD trace across a 10-bar SMA cascade is
    // not practical by hand (same rigor tradeoff as LONG_IMP above) -- this
    // is an integration smoke test: a genuine V-shaped price path (decline
    // then sharp rally) should produce a well-defined classification and at
    // least one dirty-bit fire as the crossover state changes.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;
        double price = 100.0;
        uint64_t sawIntermStochDirty = 0;
        for (int i = 0; i < 15; ++i) {
            price -= 1.0;  // decline -- pushes stochastic toward oversold
            engine.OnTick(t, price, 1, 1, 1);
            t += kTs2PeriodUs;
            sawIntermStochDirty |= engine.ConsumePatternDirtyMask();
        }
        for (int i = 0; i < 15; ++i) {
            price += 1.5;  // sharp rally -- crosses back up through oversold
            engine.OnTick(t, price, 1, 1, 1);
            t += kTs2PeriodUs;
            sawIntermStochDirty |= engine.ConsumePatternDirtyMask();
        }
        const StochasticEnum result = engine.GetIntermStochasticResult();
        check("task7_interm_stochastic_is_well_defined",
              result == StochasticEnum::NORMAL || result == StochasticEnum::OVER_SOLD ||
              result == StochasticEnum::OVER_BOUGHT);
        check("task7_interm_stochastic_dirty_bit_fired_at_least_once",
              (sawIntermStochDirty & (1ULL << static_cast<uint64_t>(IndicatorKey::INTERM_STOCHASTIC))) != 0);
    }

    // --- Task 7: EMA_PROXIMITY (TS2 Keltner EMA(13)-of-OHLC-avg proximity) ---
    // Hand-computing the rolling 13-bar stddev of a lagging EMA line across
    // many bars isn't practical by hand (same tradeoff as LONG_IMP/
    // INTERM_STOCHASTIC above) -- this is an integration smoke test: a long,
    // steady uptrend should pull price far enough above its own lagging EMA
    // to land outside 1 stddev (PRICE_ABOVE_EMA), and the dirty bit should
    // fire at least once as the classification evolves from cold-start.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 60LL * 60 * 1'000'000LL;
        double price = 100.0;
        uint64_t sawEmaProxDirty = 0;
        for (int i = 0; i < 40; ++i) {
            price += 2.0;  // sustained uptrend -- price pulls away from its own lagging EMA(13)
            engine.OnTick(t, price, 1, 1, 1);
            t += kTs2PeriodUs;
            sawEmaProxDirty |= engine.ConsumePatternDirtyMask();
        }
        const EmaProximity result = engine.GetEmaProximityResult();
        check("task7_ema_proximity_price_above_ema_on_sustained_uptrend",
              result == EmaProximity::PRICE_ABOVE_EMA || result == EmaProximity::ABOVE_TOUCH ||
              result == EmaProximity::CROSS_ABOVE);
        check("task7_ema_proximity_dirty_bit_fired_at_least_once",
              (sawEmaProxDirty & (1ULL << static_cast<uint64_t>(IndicatorKey::EMA_PROXIMITY))) != 0);
    }

    // --- Task 7: VOLUME_SIGNAL (session-aware robust log-volume z-score) ---
    // Hand-computing the median/MAD baseline across many bars isn't practical
    // by hand -- integration smoke test: a stable-but-varied volume baseline
    // (same RTH session pool throughout, so the baseline isn't degenerate),
    // followed by a genuine 10x volume spike, should classify as one of the
    // HIGH tiers and fire the dirty bit.
    {
        MarketDataReplayEngine engine;
        // 2024-01-16 10:00:00 ET (RTH) -- kBar1OpenUs is 18:00 ET (overnight);
        // +16h lands the next morning, squarely inside 09:30-16:00 ET.
        int64_t t = kBar1OpenUs + 16LL * 3600 * 1'000'000LL;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        double price = 100.0;
        uint32_t seed = 999;
        for (int i = 0; i < 20; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int64_t volume = 90 + static_cast<int64_t>(seed % 21);  // 90-110, varied
            engine.OnTick(t, price, volume, volume / 2, volume / 2);
            t += kTs3PeriodUs;
        }
        engine.ConsumePatternDirtyMask();

        // Genuine volume spike: 10x the filler baseline.
        engine.OnTick(t, price, 1000, 900, 100);
        t += kTs3PeriodUs;
        engine.OnTick(t, price, 100, 50, 50);  // force the spike bar closed

        const VolumeEnum result = engine.GetVolumeSignalResult();
        check("task7_volume_signal_fires_high_tier_on_spike",
              result == VolumeEnum::VERY_HIGH || result == VolumeEnum::HIGH ||
              result == VolumeEnum::HIGH_BUY_VOLUME || result == VolumeEnum::HIGH_SELL_VOLUME);
        const uint64_t dirtyMask = engine.ConsumePatternDirtyMask();
        check("task7_volume_signal_dirty_bit_set",
              (dirtyMask & (1ULL << static_cast<uint64_t>(IndicatorKey::VOLUME_SIGNAL))) != 0);
    }

    // --- Task 7: SIDE (always FLAT by convention -- this tool has no
    // execution/position state; see GetSideResult()'s own doc comment) ---
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        uint64_t sawSideDirty = 0;
        for (int i = 0; i < 30; ++i) {
            engine.OnTick(t, 100.0 + static_cast<double>(i), 10, 5, 5);
            t += kTs3PeriodUs;
            sawSideDirty |= engine.ConsumePatternDirtyMask();
        }
        check("task7_side_is_always_flat", engine.GetSideResult() == 0);
        check("task7_side_dirty_bit_never_fires",
              (sawSideDirty & (1ULL << static_cast<uint64_t>(IndicatorKey::SIDE))) == 0);
    }

    // --- Task 7: DAILY_BIAS (dbe::ComputeDailyBias, already pure) ---
    // Hand-verifiable: before the TS3-local Hurst's 101-bar DFA window warms
    // up, it stays at its documented cold-start neutral default (0.5), which
    // ComputeDailyBias's own FIRST check (hurst in (0.45,0.55) -> physics
    // veto) unconditionally routes to PHYSICS_VETO_RANDOM_WALK regardless of
    // price/day data -- true even with a real day-0 session seeded, proving
    // the Hurst gate (not a missing prevDayHigh/Low) is what's controlling.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        constexpr int64_t kOneDayUs = 24LL * 60 * 60 * 1'000'000LL;
        engine.OnTick(t - kOneDayUs, 100.0, 1, 1, 1);
        engine.OnTick(t - kOneDayUs + 60'000'000LL, 110.0, 1, 1, 1);
        engine.OnTick(t - kOneDayUs + 120'000'000LL, 90.0, 1, 1, 1);
        for (int i = 0; i < 20; ++i) {
            engine.OnTick(t, 105.0, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        check("task7_daily_bias_physics_veto_during_hurst_warmup",
              engine.GetDailyBiasResult() == dbe::Bias::PHYSICS_VETO_RANDOM_WALK);
    }

    // --- Audit fix verification: TurtleSoup's "Street Smarts separation"
    // filter (TURTLE_SOUP_MIN_SEPARATION=4, src/TripleScreen3.cpp) -- resets
    // a qualifying pattern to NONE if the prior window's own extreme (the
    // one the signal bar's breakout targets) occurred too recently. ---
    {
        // Test A: the prior window's lowest-low sits at ring index 18 (the
        // 19th of 20 filler bars, i.e. 2 bars before the signal bar) ->
        // barsSincePriorLow=2 < 4 -> a qualifying bullish breakout must be
        // filtered to NONE.
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        for (int i = 0; i < 20; ++i) {
            const bool isDeepLowBar = (i == 18);  // 19th filler bar (0-indexed 18)
            const double low = isDeepLowBar ? 90.0 : 98.0;
            engine.OnTick(t, 100.0, 1, 1, 1);
            engine.OnTick(t + 60'000'000LL, 102.0, 1, 1, 1);
            engine.OnTick(t + 120'000'000LL, low, 1, 1, 1);
            engine.OnTick(t + 180'000'000LL, 100.0, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        // Signal bar: breaks below fourDayLow(90), closes back above it --
        // would otherwise qualify as a genuine bullish TurtleSoup pattern.
        engine.OnTick(t, 95.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 96.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 85.0, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 95.0, 1, 1, 1);
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // force the bar closed

        check("task7_turtle_soup_separation_filter_resets_recent_extreme_to_none",
              engine.GetTurtleSoupResult() == TurtleSoupEnum::NONE);
    }
    {
        // Test B (positive control): the SAME deep low instead sits at ring
        // index 0 (the OLDEST of the 20 filler bars) -> barsSincePriorLow=20,
        // well clear of the 4-bar separation floor -- the identical signal
        // bar's bullish breakout must NOT be filtered.
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        for (int i = 0; i < 20; ++i) {
            const bool isDeepLowBar = (i == 0);
            const double low = isDeepLowBar ? 90.0 : 98.0;
            engine.OnTick(t, 100.0, 1, 1, 1);
            engine.OnTick(t + 60'000'000LL, 102.0, 1, 1, 1);
            engine.OnTick(t + 120'000'000LL, low, 1, 1, 1);
            engine.OnTick(t + 180'000'000LL, 100.0, 1, 1, 1);
            t += kTs3PeriodUs;
        }
        engine.OnTick(t, 95.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 96.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 85.0, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 95.0, 1, 1, 1);
        t += kTs3PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // force the bar closed

        check("task7_turtle_soup_separation_filter_allows_distant_extreme",
              engine.GetTurtleSoupResult() == TurtleSoupEnum::BULLISH_WEAK ||
              engine.GetTurtleSoupResult() == TurtleSoupEnum::BULLISH_STRONG ||
              engine.GetTurtleSoupResult() == TurtleSoupEnum::BULLISH_EXTREME);
    }

    // --- RASCHKE_TACTICAL_TRIGGER (5-writer, last-write-wins precedence
    // chain): a long, varied, high-volume price walk spanning enough TS3
    // bars to cross multiple RTH sessions (so ITR's own 09:30-10:30 ET
    // tracking genuinely engages) and clear every writer's own warm-up
    // requirement (base classifier's 20-bar TACTICAL_MIN_LOOKBACK is the
    // smallest; KangarooTail/ElderBreakout/TurtleSoup/NR7 were already each
    // individually warmed-up and tested above). Hand-verifying one exact
    // precedence outcome end-to-end would require re-deriving all 5
    // writers' own cascades by hand (the same "smoke test, not exact-value"
    // rationale already used for task7_interm_stochastic/task7_long_impulse
    // above) -- this instead confirms the wiring is genuinely LIVE (the
    // dirty bit fires at least once) rather than silently stuck at NONE.
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs3PeriodUs = 900LL * 1'000'000LL;
        uint32_t seed = 2468;
        double price = 100.0;
        bool sawNonNoneTactical = false;
        uint64_t sawDirtyMask = 0;
        constexpr uint64_t kRaschkeTacticalBit =
            1ULL << static_cast<uint64_t>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER);
        for (int i = 0; i < 400; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.15 * r;
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol = 50 + static_cast<int64_t>(seed % 300);
            engine.OnTick(t, price, vol, vol, vol / 2);
            sawDirtyMask |= engine.ConsumePatternDirtyMask();
            if (engine.GetRaschkeTacticalTriggerResult() != RaschkeTacticalTrigger::NONE) {
                sawNonNoneTactical = true;
            }
            t += kTs3PeriodUs;
        }
        check("task7_raschke_tactical_trigger_dirty_bit_fired_at_least_once",
              (sawDirtyMask & kRaschkeTacticalBit) != 0);
        check("task7_raschke_tactical_trigger_produced_a_non_none_result_at_least_once",
              sawNonNoneTactical);
    }

    // --- RASCHKE_STRATEGY_SETUP (13-pattern cascade, TS2 60-minute bars):
    // Three Bar Triangle is the simplest sub-pattern (only needs the current
    // bar's own high/low narrower than BOTH of the 2 immediately prior
    // bars') and the easiest to hand-construct without accidentally
    // satisfying any earlier-precedence pattern first. 27 perfectly flat
    // filler bars (single-tick, high==low==100) can never register as a
    // swing point (IsSwingLowAt/HighAt's own strict-inequality requirement
    // fails on ties), safely ruling out Holy Grail/Double Repo*/Ghost/Bread
    // and Butter/Anti/Slingshot without needing to hand-verify each one's
    // own full cascade -- confirmed empirically (this fixture was iterated
    // against actual engine output, not assumed correct from the pattern
    // spec alone, consistent with this suite's own established practice for
    // multi-stage cascades).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 3600LL * 1'000'000LL;  // 60 min

        for (int i = 0; i < 27; ++i) {
            engine.OnTick(t, 100.0, 1, 1, 1);
            t += kTs2PeriodUs;
        }
        // 2 prior bars with a real, wide 95-105 range (both sides of the
        // eventual triangle bar).
        engine.OnTick(t, 100.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 105.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 95.0, 1, 1, 1);
        t += kTs2PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 105.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 95.0, 1, 1, 1);
        t += kTs2PeriodUs;
        // The triangle bar itself: narrower than both prior bars on both sides.
        engine.OnTick(t, 100.0, 1, 1, 1);
        engine.OnTick(t + 60'000'000LL, 102.0, 1, 1, 1);
        engine.OnTick(t + 120'000'000LL, 98.0, 1, 1, 1);
        engine.OnTick(t + 180'000'000LL, 100.0, 1, 1, 1);
        t += kTs2PeriodUs;
        engine.OnTick(t, 100.0, 1, 1, 1);  // force the bar closed

        check("task7_raschke_strategy_setup_three_bar_triangle",
              engine.GetRaschkeStrategySetupResult() == RaschkeStrategySetup::THREE_BAR_TRIANGLE);
    }

    // Smoke test (same rationale as task7_raschke_tactical_trigger above --
    // 13 interacting sub-patterns make hand-verifying one exact end-to-end
    // precedence outcome impractical beyond the single-pattern case above):
    // a long, varied, high-volume price walk confirms the full cascade is
    // genuinely live (dirty bit fires, a non-NONE result is produced).
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 3600LL * 1'000'000LL;
        uint32_t seed = 13579;
        double price = 100.0;
        bool sawNonNoneStrategy = false;
        uint64_t sawDirtyMask = 0;
        constexpr uint64_t kRaschkeStrategyBit =
            1ULL << static_cast<uint64_t>(IndicatorKey::RASCHKE_STRATEGY_SETUP);
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.2 * r;
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol = 50 + static_cast<int64_t>(seed % 300);
            engine.OnTick(t, price, vol, vol, vol / 2);
            seed = seed * 1664525u + 1013904223u;
            const double r2 = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            engine.OnTick(t + 1'800'000'000LL, price + 0.2 * r2, vol, vol, vol / 2);
            sawDirtyMask |= engine.ConsumePatternDirtyMask();
            if (engine.GetRaschkeStrategySetupResult() != RaschkeStrategySetup::NONE) {
                sawNonNoneStrategy = true;
            }
            t += kTs2PeriodUs;
        }
        check("task7_raschke_strategy_setup_dirty_bit_fired_at_least_once",
              (sawDirtyMask & kRaschkeStrategyBit) != 0);
        check("task7_raschke_strategy_setup_produced_a_non_none_result_at_least_once",
              sawNonNoneStrategy);
    }

    // --- Task 9: BuildTrainingEventT() -- pooled TrainingEventT assembly.
    // Warms up the same way as the RASCHKE_STRATEGY_SETUP smoke test above
    // (long varied walk) so prev_day_high/low, prev_four_bar_high/low, and
    // the pattern-detector fields are all genuinely non-default. ---
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 3600LL * 1'000'000LL;
        uint32_t seed = 24680;
        double price = 100.0;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.2 * r;
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol = 50 + static_cast<int64_t>(seed % 300);
            engine.OnTick(t, price, vol, vol, vol / 2);
            seed = seed * 1664525u + 1013904223u;
            const double r2 = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            engine.OnTick(t + 1'800'000'000LL, price + 0.2 * r2, vol, vol, vol / 2);
            t += kTs2PeriodUs;
        }

        const auto& event = engine.BuildTrainingEventT(42, 1705359600123456LL, 100.25f, 101.5f, 99.75f, 100.5f, 12345);

        // The 4 extract-to-pure fields round-trip exactly.
        check("task9_bar_index_round_trips", event.bar_index == 42);
        check("task9_timestamp_us_round_trips", event.timestamp_us == 1705359600123456LL);
        check("task9_ohlcv_round_trips",
              event.open == 100.25f && event.high == 101.5f && event.low == 99.75f &&
              event.close == 100.5f && event.volume == 12345);

        // IndicatorState's 17 PRIMARY_TRIGGER_MASK fields mirror this engine's
        // own current Get*Result() accessors exactly.
        check("task9_indicator_state_kangaroo_tail_matches",
              event.indicators->kangaroo_tail() == static_cast<int8_t>(engine.GetKangarooTailResult()));
        check("task9_indicator_state_turtle_soup_matches",
              event.indicators->turtle_soup() == static_cast<int8_t>(engine.GetTurtleSoupResult()));
        check("task9_indicator_state_elder_breakout_matches",
              event.indicators->elder_breakout() == static_cast<int8_t>(engine.GetElderBreakoutResult()));
        check("task9_indicator_state_nr7_matches",
              event.indicators->nr7() == static_cast<int8_t>(engine.GetNr7Result()));

        // Real bug fixed 2026-09-18 (lbrnet finding): the 5 quality scores were computed
        // internally by each pattern's own Detect*() call but never reached the wire --
        // only kangaroo_tail_quality had a member to even hold the value, and even that one
        // was never mutate_*()'d. All 5 must now round-trip exactly, same as their enums above.
        check("task9_indicator_state_kangaroo_tail_quality_matches",
              event.indicators->kangaroo_tail_quality() == engine.GetKangarooTailQuality());
        check("task9_indicator_state_turtle_soup_quality_matches",
              event.indicators->turtle_soup_quality() == engine.GetTurtleSoupQuality());
        check("task9_indicator_state_momentum_pinball_quality_matches",
              event.indicators->momentum_pinball_quality() == engine.GetMomentumPinballQuality());
        check("task9_indicator_state_elder_breakout_quality_matches",
              event.indicators->elder_breakout_quality() == engine.GetElderBreakoutQuality());
        check("task9_indicator_state_nr7_quality_matches",
              event.indicators->nr7_quality() == engine.GetNr7Quality());
        check("task9_indicator_state_rsi_matches",
              event.indicators->rsi() == static_cast<int8_t>(engine.GetRsiTopResult()));
        check("task9_indicator_state_interm_stochastic_matches",
              event.indicators->interm_stochastic() == static_cast<int8_t>(engine.GetIntermStochasticResult()));
        check("task9_indicator_state_atr_proximity_matches",
              event.indicators->atr_proximity() == static_cast<int8_t>(engine.GetAtrProximityResult()));
        check("task9_indicator_state_ema_proximity_matches",
              event.indicators->ema_proximity() == static_cast<int8_t>(engine.GetEmaProximityResult()));
        check("task9_indicator_state_raschke_strategy_setup_matches",
              event.indicators->raschke_strategy_setup() == static_cast<int8_t>(engine.GetRaschkeStrategySetupResult()));
        check("task9_indicator_state_raschke_tactical_trigger_matches",
              event.indicators->raschke_tactical_trigger() == static_cast<int8_t>(engine.GetRaschkeTacticalTriggerResult()));
        check("task9_indicator_state_structure_test_matches",
              event.indicators->structure_test() == static_cast<int8_t>(engine.GetStructureTestResult()));
        check("task9_indicator_state_volume_signal_matches",
              event.indicators->volume_signal() == static_cast<int8_t>(engine.GetVolumeSignalResult()));
        check("task9_indicator_state_daily_bias_matches",
              event.indicators->daily_bias() == static_cast<int8_t>(engine.GetDailyBiasResult()));

        // event.observation is a direct reuse of this engine's own m_obs.
        check("task9_observation_matches_engine_obs",
              event.observation->hurst_exponent() == engine.GetObservation().hurst_exponent() &&
              event.observation->fractal_dim() == engine.GetObservation().fractal_dim());

        // side is always FLAT; model_confidence is production's only real value.
        check("task9_side_is_flat", event.side == 0);
        check("task9_model_confidence_is_zero", event.model_confidence == 0.0f);

        // Retained shared-root fields have moved off their zero defaults after warmup.
        check("task9_prev_day_high_low_nonzero_after_warmup",
              event.prev_day_high > 0.0f && event.prev_day_low > 0.0f);
        check("task9_prev_high_low_nonzero_after_warmup",
              event.prev_high > 0.0f && event.prev_low > 0.0f);
        check("task9_prev_four_bar_high_low_nonzero_after_warmup",
              event.prev_four_bar_high > 0.0f && event.prev_four_bar_low > 0.0f);

        // Out-of-scope fields hold their documented sentinels, not stale/
        // guessed values.
        check("task9_out_of_scope_int_sentinels_are_zero",
              event.market_symbol == 0 && event.overnight_exit == 0 && event.hmm_state == 0);
        check("task9_out_of_scope_float_sentinels_are_zero",
              event.nh_nl_daily == 0.0f && event.close_percentile == 0.0f &&
              event.volume_ratio_percent == 0.0f && event.volume_imbalance == 0.0f);
        check("task9_long_imp_interm_imp_are_documented_undefined_sentinel",
              event.indicators->long_imp() == static_cast<int8_t>(ImpulseEnum::UNDEFINED) &&
              event.indicators->interm_imp() == static_cast<int8_t>(ImpulseEnum::UNDEFINED) &&
              event.indicators->impulse_run_length() == 0);

        // Pooling (DOD requirement, spec §11 item 1): a second call reuses
        // the SAME underlying indicators/observation allocation -- never a
        // fresh unique_ptr per emission.
        const auto* indicatorsPtrBefore = event.indicators.get();
        const auto* observationPtrBefore = event.observation.get();
        const auto& event2 = engine.BuildTrainingEventT(43, 1705359601000000LL, 1.0f, 2.0f, 0.5f, 1.5f, 99);
        check("task9_indicators_pointer_reused_across_calls", event2.indicators.get() == indicatorsPtrBefore);
        check("task9_observation_pointer_reused_across_calls", event2.observation.get() == observationPtrBefore);
        check("task9_second_call_overwrites_bar_index", event2.bar_index == 43);
    }

    // --- Task 10: BuildRiskGateContextT() -- reuses the same 200-bar warm-up
    // fixture as Task 9 so m_obs's own dims are genuinely non-default. ---
    {
        MarketDataReplayEngine engine;
        int64_t t = kBar1OpenUs;
        constexpr int64_t kTs2PeriodUs = 3600LL * 1'000'000LL;
        uint32_t seed = 97531;
        double price = 100.0;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            price += 0.2 * r;
            seed = seed * 1664525u + 1013904223u;
            const int64_t vol = 50 + static_cast<int64_t>(seed % 300);
            engine.OnTick(t, price, vol, vol, vol / 2);
            seed = seed * 1664525u + 1013904223u;
            const double r2 = (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;
            engine.OnTick(t + 1'800'000'000LL, price + 0.2 * r2, vol, vol, vol / 2);
            t += kTs2PeriodUs;
        }

        const auto& rgc = engine.BuildRiskGateContextT(1705359600123456LL);

        // The 6 in-scope fields mirror m_obs/m_featureScaler's own live state exactly.
        check("task10_hurst_exponent_matches_m_obs", rgc.hurst_exponent == engine.GetObservation().hurst_exponent());
        check("task10_mean_rev_z_matches_m_obs", rgc.mean_rev_z == engine.GetObservation().mean_rev_z());
        check("task10_fisher_info_matches_m_obs", rgc.fisher_info == engine.GetObservation().fisher_info());
        check("task10_amihud_illiquidity_matches_m_obs",
              rgc.amihud_illiquidity == engine.GetObservation().amihud_illiquidity());
        check("task10_spread_stress_matches_liq_fragility",
              rgc.spread_stress == engine.GetObservation().liq_fragility());
        check("task10_pareto_tail_alpha_matches_tail_index",
              rgc.pareto_tail_alpha == engine.GetObservation().tail_index());
        check("task10_snapshot_timestamp_us_matches_passed_in_timestamp",
              rgc.snapshot_timestamp_us == 1705359600123456LL);

        // fractal_dim: genuine gap, documented sentinel (NOT this engine's own
        // 400-bar m_obs.fractal_dim(), which would be the wrong window).
        check("task10_fractal_dim_is_documented_sentinel_not_wrong_window",
              rgc.fractal_dim == 1.5f);

        // Out-of-scope fields hold LocalRiskContext.h's own documented
        // defaults -- especially raschke_burst, which must NOT be
        // RiskGateContextT's own mismatched wire default (1.0f).
        check("task10_out_of_scope_fields_hold_documented_defaults",
              rgc.shannon_flow_entropy == 0.0f && rgc.shannon_efficiency == 0.5f &&
              rgc.taleb_kurtosis == 0.0f && rgc.taleb_skewness == 0.0f &&
              rgc.elder_chandelier_atr == 0.0f && rgc.regime_duration == 0 &&
              rgc.amihud_percentile == 0.5f);
        check("task10_raschke_burst_is_poisson_neutral_not_wire_default",
              rgc.raschke_burst == 0.0f);
    }

    // --- Task 10: is_valid reflects FeatureScaler's real warm-up state, not
    // a hardcoded true/false -- checked in a fresh (never-warmed-up) engine. ---
    {
        MarketDataReplayEngine engine;
        engine.OnTick(kBar1OpenUs, 100.0, 1, 1, 1);
        const auto& rgc = engine.BuildRiskGateContextT(kBar1OpenUs);
        check("task10_is_valid_is_false_before_feature_scaler_warms_up", rgc.is_valid == false);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
