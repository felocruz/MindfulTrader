// tools/observation_vector/volatility_dim_redundancy_eval.cpp
// Offline, model-independent redundancy check for the "volatility level"
// axis's log_scale_ratio and log_scale_expansion_ratio (was correction_action),
// each in OLD (raw-variance/RV) and NEW (bipower-variation) form --
// lbrnet/logs/rc_gemini.log CLAUDE_BRIEF_118/119 and replies.
//
// Pairwise Pearson correlation among all four series, most importantly the
// NEW-vs-NEW ("both fat-tail-compliant") comparison: classical measurement
// theory (correction for attenuation, Spearman 1904) predicts that removing
// jump noise from BOTH sides should raise their correlation, not lower it --
// each OLD/NEW pair is the same true short-vs-long volatility-ratio signal
// with different amounts of noise riding on it, and de-noising both should
// reveal a higher true correlation than any OLD-involving comparison shows.
// relative_range is intentionally excluded -- see volatility_dim_stats.h's
// own header comment for why (needs High/Low bar data this tick-close
// pipeline doesn't carry).
//
// This system's Student-t HMM uses DIAGONAL covariance (lbrnet/lbrnet/models/
// student_t_hmm.py hard-enforces covariance_type='diag'), so the classic
// "correlated features -> ill-conditioned Sigma_k^-1" failure mode this
// tool's condition-number diagnostic was originally built to check does not
// apply here -- a diagonal model never estimates cross-dimensional
// covariance in the first place. The condition number is still printed
// (kept generic/reusable, e.g. for a future full-covariance context), but
// the raw pairwise correlations are the directly relevant numbers for THIS
// system: under diagonal covariance / conditional-independence, correlated
// "independent" dims cause double-counted evidence (the naive-Bayes
// overconfidence problem), not numerical singularity.
//
// Deliberately does NOT attempt an HMM-state-discrimination measurement --
// see this file's git history/commit message for the full reasoning
// (requires an already-fitted model, which is circular while the vector
// itself is still being fixed).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/observation_vector/volatility_dim_redundancy_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/volatility_dim_redundancy_eval
#include "drift_location_stats.h"  // for ComputeLogReturns
#include "market_data_io.h"
#include "volatility_dim_stats.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--long-window 200] [--expansion-window 100]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path;
    int long_window = 200;
    int expansion_window = 100;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--long-window") long_window = std::stoi(next("--long-window"));
        else if (arg == "--expansion-window") expansion_window = std::stoi(next("--expansion-window"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticks_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    TickSeries series;
    try {
        series = ReadTicksParquet(ticks_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "\xE2\x9D\x8C %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu rows from %s\n", series.timestamp_us.size(), ticks_path.c_str());
    std::fflush(stdout);

    const auto log_returns = ComputeLogReturns(series.close);
    const std::size_t n = log_returns.size();

    std::printf("Computing log_scale_ratio (OLD/NEW, long_window=%d) and "
                "log_scale_expansion_ratio (OLD/NEW, window=%d) over %zu ticks...\n",
                long_window, expansion_window, n);
    std::fflush(stdout);
    std::vector<double> lsr_old(n), lsr_new(n), lser_old(n), lser_new(n);
    for (std::size_t i = 0; i < n; ++i) {
        lsr_old[i] = ComputeLogScaleRatioOld(log_returns, i, long_window);
        lsr_new[i] = ComputeLogScaleRatioNew(log_returns, i, long_window);
        lser_old[i] = ComputeLogScaleExpansionRatioOld(log_returns, i, expansion_window);
        lser_new[i] = ComputeLogScaleExpansionRatioNew(log_returns, i, expansion_window);
    }
    std::printf("Done computing feature series.\n");
    std::fflush(stdout);

    std::printf("\n=== Redundancy: pairwise Pearson correlation ===\n");
    const double c_old_old = PearsonCorrelation(lsr_old, lser_old);
    const double c_old_new = PearsonCorrelation(lsr_old, lser_new);
    const double c_new_old = PearsonCorrelation(lsr_new, lser_old);
    const double c_new_new = PearsonCorrelation(lsr_new, lser_new);
    std::printf("  corr(log_scale_ratio_OLD, log_scale_expansion_ratio_OLD) = %+.4f  (both noisy)\n", c_old_old);
    std::printf("  corr(log_scale_ratio_OLD, log_scale_expansion_ratio_NEW) = %+.4f\n", c_old_new);
    std::printf("  corr(log_scale_ratio_NEW, log_scale_expansion_ratio_OLD) = %+.4f\n", c_new_old);
    std::printf("  corr(log_scale_ratio_NEW, log_scale_expansion_ratio_NEW) = %+.4f  (both fat-tail-compliant)\n", c_new_new);
    std::printf("\n  [de-attenuation check] both-clean minus both-noisy correlation = %+.4f "
                "(positive => de-noising both sides revealed a higher true correlation, as predicted "
                "by correction-for-attenuation, Spearman 1904)\n", c_new_new - c_old_old);
    std::fflush(stdout);

    const double cond_2x2_clean = (1.0 + std::fabs(c_new_new)) / std::max(1e-9, 1.0 - std::fabs(c_new_new));
    const double cond_2x2_noisy = (1.0 + std::fabs(c_old_old)) / std::max(1e-9, 1.0 - std::fabs(c_old_old));
    std::printf("\n  2x2 condition number (both-clean pair)  = %.2f\n", cond_2x2_clean);
    std::printf("  2x2 condition number (both-noisy pair)  = %.2f\n", cond_2x2_noisy);
    std::printf("  NOTE: this system's Student-t HMM uses DIAGONAL covariance (hard-enforced in\n"
                "  lbrnet/lbrnet/models/student_t_hmm.py) -- it never estimates cross-dimensional\n"
                "  covariance, so a Sigma_k ill-conditioning failure mode cannot occur regardless of\n"
                "  this number. The condition number is printed for reference only; the raw\n"
                "  correlation above is the directly relevant diagnostic for THIS architecture\n"
                "  (correlated dims under conditional independence double-count evidence rather\n"
                "  than breaking a matrix inversion).\n");
    std::fflush(stdout);

    return 0;
}
