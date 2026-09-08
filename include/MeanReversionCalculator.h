// MeanReversionCalculator.h — pure, header-only mean-reversion elasticity score
// (dim 16, mean_rev_z), extracted from StudyHelperFunctions.cpp's
// CalculateMeanReversionSpeed so it can be natively unit-tested
// (tests/cpp/test_mean_reversion_calculator.cpp) and reused by an offline,
// non-Sierra-Chart tool building the same 18D ObservationData vector directly
// from tick data. Same rationale/precedent as CarryForwardCalculators.h/
// OrderFlowAsymmetryEngine.h/SevcikFractalDimension.h.
//
// The caller (StudyHelperFunctions.cpp) owns the ACSIL price pull (gathering
// the chronological price window ending at the current bar) and the
// persistent-state carry-forward; this file owns only the math: median/MAD
// (Kim & White 2004) price z-score, suppressed by lag-1 return
// autocorrelation elasticity gating.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mrc {

// Matches the [10,40] adaptive observation window contract's upper bound
// (CalculateAdaptiveObservationWindow's own std::clamp(..., 10, 40)) --
// defensive upper bound so the fixed-capacity scratch buffers below can
// never be written out of range.
constexpr int kMaxLookback = 40;

// prices[0..n-1]: chronological price window, prices[n-1] == current
// (still-forming) bar's price. n in [4, kMaxLookback] (n<4 gives m<3,
// skipping the autocorrelation term entirely -- degenerate but not invalid).
// Degenerate (flat window, MAD collapses below the numerical floor) carries
// lastValidValue forward instead of a fabricated exact-zero "no stretch"
// reading. Contract: result in [0, 5].
inline float ComputeMeanReversionZ(const float* prices, int n, float lastValidValue) {
    constexpr double kMadConsistency = 1.4826;
    constexpr double kPriceEps = 1e-6;

    std::array<double, kMaxLookback> log_prices{};
    for (int i = 0; i < n; ++i) {
        const double p = std::max(static_cast<double>(prices[i]), kPriceEps);
        log_prices[static_cast<size_t>(i)] = std::log(p);
    }

    std::array<double, kMaxLookback> scratch{};
    std::copy_n(log_prices.begin(), n, scratch.begin());
    const int priceMid = n / 2;
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double median_log_p = scratch[static_cast<size_t>(priceMid)];

    for (int i = 0; i < n; ++i) {
        scratch[static_cast<size_t>(i)] = std::abs(log_prices[static_cast<size_t>(i)] - median_log_p);
    }
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double mad_log_p = scratch[static_cast<size_t>(priceMid)];
    const double scale_log_p = mad_log_p * kMadConsistency;

    if (scale_log_p < 1e-6) {
        return lastValidValue;
    }

    const double current_log_p = std::log(std::max(static_cast<double>(prices[n - 1]), kPriceEps));
    const double abs_z_price = std::abs((current_log_p - median_log_p) / scale_log_p);

    // Lag-1 autocorrelation on log-returns: positive rho => momentum, negative => reversion.
    const int m = n - 1;
    if (m < 3) {
        // Too few samples for the autocorrelation term -- genuinely computed,
        // not degenerate, just skips the elasticity gate.
        return std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f);
    }

    std::array<double, kMaxLookback> returns{};
    for (int i = 0; i < m; ++i) {
        const double p = std::max(static_cast<double>(prices[i + 1]), kPriceEps);
        const double p_prev = std::max(static_cast<double>(prices[i]), kPriceEps);
        returns[static_cast<size_t>(i)] = std::log(p / p_prev);
    }

    std::array<double, kMaxLookback> returnScratch{};
    std::copy_n(returns.begin(), m, returnScratch.begin());
    const int retMid = m / 2;
    std::nth_element(returnScratch.begin(), returnScratch.begin() + retMid, returnScratch.begin() + m);
    const double median_r = returnScratch[static_cast<size_t>(retMid)];

    double num = 0.0;
    double den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[static_cast<size_t>(t)] - median_r;
        const double r_prev = returns[static_cast<size_t>(t - 1)] - median_r;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }

    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticity_gate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    const double score = abs_z_price * elasticity_gate;

    return std::clamp(static_cast<float>(score), 0.0f, 5.0f);
}

}  // namespace mrc
