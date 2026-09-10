// FeatureSaliencyEM.h — Gaussian mixture + Feature Saliency EM fitter,
// Phase 2a (Elite Feature Set Curation, Phase 2). Pure numerical core: no
// I/O, no Arrow/Parquet dependency, unit-testable on synthetic data alone.
// docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md.
//
// Law, Figueiredo & Jain (2004), "Simultaneous Feature Selection and
// Clustering Using Mixture Models," IEEE TPAMI 26(9):1154-1166. Diagonal
// covariance, K Gaussian components, D features, hard MDL/BIC saliency-
// pruning gate (spec §2.4 -- confirmed required, not optional; see
// ApplyHardSaliencyPruning's own comment and
// knowledge/global/cpp/feature_saliency_em_mml_pruning.md for why a
// continuous soft-thresholding attempt was superseded by this hard gate),
// no transition matrix (static mixture, not a true HMM, per spec §2.5).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace fsem {

// D is a compile-time template parameter (matches this repo's own
// otg::kObservationDim-style fixed-size convention) -- avoids heap
// allocation per observation.
template <std::size_t D>
using Observation = std::array<double, D>;

// --- Task 1: k-means++ seeding (Arthur & Vassilvitskii 2007) ---
//
// Ported from student_t_hmm.py's own _kmeans_plusplus_seed_indices
// algorithm (same method, not a different one, per spec §5): first seed
// uniform-random, each subsequent seed sampled with probability
// proportional to its squared distance to the nearest already-chosen seed.
template <std::size_t D>
std::vector<std::size_t> KMeansPlusPlusSeed(
    const std::vector<Observation<D>>& observations, std::size_t K, std::uint64_t rngSeed) {
    std::vector<std::size_t> seeds;
    if (observations.empty() || K == 0) return seeds;
    std::mt19937_64 rng(rngSeed);

    auto sqDist = [](const Observation<D>& a, const Observation<D>& b) -> double {
        double s = 0.0;
        for (std::size_t j = 0; j < D; ++j) {
            const double d = a[j] - b[j];
            s += d * d;
        }
        return s;
    };

    const std::size_t n = observations.size();
    std::uniform_int_distribution<std::size_t> uni(0, n - 1);
    seeds.push_back(uni(rng));

    std::vector<double> minSqDist(n, std::numeric_limits<double>::max());
    while (seeds.size() < K && seeds.size() < n) {
        const auto& lastSeed = observations[seeds.back()];
        double totalWeight = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            minSqDist[i] = std::min(minSqDist[i], sqDist(observations[i], lastSeed));
            totalWeight += minSqDist[i];
        }
        if (totalWeight <= 0.0) {
            // All remaining points are coincident with an already-chosen seed --
            // fall back to uniform sampling rather than dividing by zero.
            seeds.push_back(uni(rng));
            continue;
        }
        std::uniform_real_distribution<double> unitDist(0.0, totalWeight);
        const double target = unitDist(rng);
        double cumulative = 0.0;
        std::size_t chosen = n - 1;
        for (std::size_t i = 0; i < n; ++i) {
            cumulative += minSqDist[i];
            if (cumulative >= target) {
                chosen = i;
                break;
            }
        }
        seeds.push_back(chosen);
    }
    return seeds;
}

// --- Task 2: E-step (spec §2.1/§2.2) ---

// Per-component/per-feature parameters, plus per-feature saliency phi_j and
// mixture weights pi_k. K is a runtime size (pi.size()); D is the compile-
// time observation dimensionality.
template <std::size_t D>
struct FitParams {
    std::vector<double> pi;                 // size K
    std::vector<Observation<D>> stateMean;  // size K
    std::vector<Observation<D>> stateVar;   // size K
    Observation<D> bgMean{};
    Observation<D> bgVar{};
    Observation<D> phi{};                   // saliency, one per feature

    std::size_t K() const { return pi.size(); }
};

inline double GaussianDensity(double x, double mean, double var) {
    // Guards a component/background variance from collapsing to zero
    // (matches this repo's own kVarEps-style convention, e.g.
    // ObservationTriggerGate.h's own safe_variance floor).
    constexpr double kVarEps = 1e-9;
    constexpr double kTwoPi = 6.283185307179586;
    const double v = std::max(var, kVarEps);
    const double diff = x - mean;
    return std::exp(-0.5 * diff * diff / v) / std::sqrt(kTwoPi * v);
}

// p(x|k) = Prod_j [ phi_j*f(x_j|theta_kj) + (1-phi_j)*q(x_j|lambda_j) ] (spec §2.1).
template <std::size_t D>
double ComponentDensity(const Observation<D>& x, std::size_t k, const FitParams<D>& params) {
    double p = 1.0;
    for (std::size_t j = 0; j < D; ++j) {
        const double salient = GaussianDensity(x[j], params.stateMean[k][j], params.stateVar[k][j]);
        const double background = GaussianDensity(x[j], params.bgMean[j], params.bgVar[j]);
        p *= params.phi[j] * salient + (1.0 - params.phi[j]) * background;
    }
    return p;
}

// w_{i,k} = P(s_i=k | x_i), standard Bayes over pi_k*p(x_i|k) (spec §2.2).
template <std::size_t D>
std::vector<std::vector<double>> ComputeStateResponsibilities(
    const std::vector<Observation<D>>& observations, const FitParams<D>& params) {
    const std::size_t K = params.K();
    std::vector<std::vector<double>> w(observations.size(), std::vector<double>(K, 0.0));
    // Floor avoids a 0/0 divide when every component's density has
    // underflowed to zero (all observations degenerate/identical) -- matches
    // this repo's own "guard, don't crash" convention for edge-case inputs.
    constexpr double kEps = 1e-300;
    for (std::size_t i = 0; i < observations.size(); ++i) {
        std::vector<double> unnorm(K);
        double total = 0.0;
        for (std::size_t k = 0; k < K; ++k) {
            unnorm[k] = params.pi[k] * ComponentDensity(observations[i], k, params);
            total += unnorm[k];
        }
        const double safeTotal = std::max(total, kEps);
        for (std::size_t k = 0; k < K; ++k) {
            w[i][k] = unnorm[k] / safeTotal;
        }
    }
    return w;
}

// u_{i,k,j} = P(z_{i,j}=1 | x_i, s_i=k) -- salient-vs-background branch split
// for feature j, given the observation is (hypothetically) in state k
// (spec §2.2).
template <std::size_t D>
std::vector<std::vector<Observation<D>>> ComputeFeatureResponsibilities(
    const std::vector<Observation<D>>& observations, const FitParams<D>& params) {
    const std::size_t K = params.K();
    constexpr double kEps = 1e-300;
    std::vector<std::vector<Observation<D>>> u(observations.size(), std::vector<Observation<D>>(K));
    for (std::size_t i = 0; i < observations.size(); ++i) {
        for (std::size_t k = 0; k < K; ++k) {
            for (std::size_t j = 0; j < D; ++j) {
                const double salient = GaussianDensity(
                    observations[i][j], params.stateMean[k][j], params.stateVar[k][j]);
                const double background = GaussianDensity(
                    observations[i][j], params.bgMean[j], params.bgVar[j]);
                const double num = params.phi[j] * salient;
                const double denom = num + (1.0 - params.phi[j]) * background;
                u[i][k][j] = num / std::max(denom, kEps);
            }
        }
    }
    return u;
}

// --- Task 3: M-step (spec §2.3) -- closed-form, no root-finding ---
//
// Given known responsibilities w_{i,k}/u_{i,k,j}, produces updated params.
// Per-state/per-feature stats are weighted by BOTH responsibilities (only
// the salient fraction informs the state-specific parameter); background
// stats are weighted by the COMPLEMENT (1-u), summed over all states.
template <std::size_t D>
FitParams<D> ComputeMStepUpdate(
    const std::vector<Observation<D>>& observations,
    const std::vector<std::vector<double>>& w,
    const std::vector<std::vector<Observation<D>>>& u) {
    const std::size_t N = observations.size();
    const std::size_t K = w.empty() ? 0 : w[0].size();
    // Guards a component/feature whose responsibility mass has collapsed to
    // ~0 (e.g. an empty cluster) from a 0/0 divide -- same posture as
    // ObservationTriggerGate.h's own kVarEps floor.
    constexpr double kWeightEps = 1e-12;
    constexpr double kVarEps = 1e-9;

    FitParams<D> params;
    params.pi.assign(K, 0.0);
    params.stateMean.assign(K, Observation<D>{});
    params.stateVar.assign(K, Observation<D>{});

    for (std::size_t k = 0; k < K; ++k) {
        double sumW = 0.0;
        for (std::size_t i = 0; i < N; ++i) sumW += w[i][k];
        params.pi[k] = sumW / static_cast<double>(N > 0 ? N : 1);

        for (std::size_t j = 0; j < D; ++j) {
            double num = 0.0, denom = 0.0;
            for (std::size_t i = 0; i < N; ++i) {
                const double weight = w[i][k] * u[i][k][j];
                num += weight * observations[i][j];
                denom += weight;
            }
            const double safeDenom = std::max(denom, kWeightEps);
            const double mean = num / safeDenom;
            double varNum = 0.0;
            for (std::size_t i = 0; i < N; ++i) {
                const double weight = w[i][k] * u[i][k][j];
                const double diff = observations[i][j] - mean;
                varNum += weight * diff * diff;
            }
            params.stateMean[k][j] = mean;
            params.stateVar[k][j] = std::max(varNum / safeDenom, kVarEps);
        }
    }

    for (std::size_t j = 0; j < D; ++j) {
        double num = 0.0, denom = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t k = 0; k < K; ++k) {
                const double weight = w[i][k] * (1.0 - u[i][k][j]);
                num += weight * observations[i][j];
                denom += weight;
            }
        }
        const double safeDenom = std::max(denom, kWeightEps);
        const double mean = num / safeDenom;
        double varNum = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t k = 0; k < K; ++k) {
                const double weight = w[i][k] * (1.0 - u[i][k][j]);
                const double diff = observations[i][j] - mean;
                varNum += weight * diff * diff;
            }
        }
        params.bgMean[j] = mean;
        params.bgVar[j] = std::max(varNum / safeDenom, kVarEps);
    }

    for (std::size_t j = 0; j < D; ++j) {
        double sum = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t k = 0; k < K; ++k) sum += w[i][k] * u[i][k][j];
        }
        params.phi[j] = sum / static_cast<double>(N > 0 ? N : 1);
    }

    return params;
}

// --- MDL/BIC hard saliency-pruning gate (spec §2.4) ---
//
// NOT optional -- confirmed empirically (knowledge/global/cpp/
// feature_saliency_em_mml_pruning.md): raw likelihood-only phi_j
// (ComputeMStepUpdate above) has no mechanism that can ever push a
// genuinely irrelevant feature's saliency toward 0 -- the salient branch (K
// per-state parameter pairs) can never fit worse than the shared background
// branch (1 parameter pair), so likelihood alone always weakly prefers
// phi_j=1 regardless of true relevance.
//
// SUPERSEDES an earlier continuous soft-thresholding attempt (subtract a
// flat c/(2N)-scale penalty from phi_j every M-step). A second independent
// Gemini CLI read-only literature consult (2026-09-09) diagnosed that
// approach as structurally wrong, not just miscalibrated: a flat O(1)
// penalty can never overcome the O(N) responsibility-mass advantage a
// K-Gaussian salient branch has when merely overfitting noise, so phi_j for
// a genuinely irrelevant feature never approached 0 at any sample size
// tested (empirical sweep: knowledge chunk's own table). True MDL/BIC
// structural (L0) model selection needs a penalty that scales with ln(N),
// evaluated as an explicit compare-before-prune decision -- not a
// continuous per-iteration shrinkage.
//
// This function implements that hard gate, applied ONCE after the raw EM
// loop (below) converges -- NOT during the loop, so the loop stays plain,
// textbook EM with its full log-likelihood ascent guarantee intact (no
// objective-function mismatch with the convergence check, unlike the
// superseded approach). Per-feature BIC comparison of two nested models for
// feature j's marginal density: "salient" (K state-conditional Gaussians,
// mixed by the converged state responsibilities w_{i,k}) vs "background"
// (1 shared Gaussian) -- degrees-of-freedom difference is 2K-2 (K
// (mean,variance) pairs vs 1), so the BIC decision rule
// `2*(LL_salient-LL_background) > (2K-2)*ln(N)` simplifies to:
//
//   LL_salient_j - LL_background_j  <  (K-1) * ln(N)   =>  prune phi_j to 0
//
// (Wilks' theorem: under the null "feature j is truly background-only," the
// log-likelihood gain from the more-flexible K-Gaussian fit is
// chi-squared-distributed with K-1 degrees of freedom, so its expectation
// is O(1) regardless of N -- while the ln(N) threshold grows without bound,
// guaranteeing the gate eventually overwhelms pure-noise overfitting at any
// sample size, unlike the superseded flat-constant approach.) Features that
// pass the gate keep their raw M-step phi_j (already near 1 for genuinely
// salient features); this function only ever moves a value DOWN to exactly
// 0, never up.
//
// The "background" null-hypothesis density is deliberately the GLOBAL
// (unweighted, single-Gaussian-over-all-N-observations) mean/variance for
// feature j, NOT params.bgMean/bgVar -- a real bug found empirically: the
// EM-fitted bgMean/bgVar are themselves computed via the (1-u)-weighted
// M-step, so as phi_j rises during the raw EM phase, that weight collapses
// toward near-zero mass and the "background" density degrades/narrows
// alongside it, no longer representing a fair null hypothesis. The global,
// unweighted per-feature mean/variance is uncorrupted by phi and is the
// correct null model for this specific comparison.
template <std::size_t D>
void ApplyHardSaliencyPruning(
    FitParams<D>& params, const std::vector<Observation<D>>& observations,
    const std::vector<std::vector<double>>& w,
    const Observation<D>& globalMean, const Observation<D>& globalVar) {
    const std::size_t N = observations.size();
    const std::size_t K = params.K();
    if (N == 0 || K == 0) return;
    constexpr double kEps = 1e-300;
    const double penalty = (static_cast<double>(K) - 1.0) * std::log(static_cast<double>(N));
    for (std::size_t j = 0; j < D; ++j) {
        double llSalient = 0.0, llBackground = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            double mixSalient = 0.0;
            for (std::size_t k = 0; k < K; ++k) {
                mixSalient += w[i][k] * GaussianDensity(
                    observations[i][j], params.stateMean[k][j], params.stateVar[k][j]);
            }
            llSalient += std::log(std::max(mixSalient, kEps));
            llBackground += std::log(std::max(
                GaussianDensity(observations[i][j], globalMean[j], globalVar[j]), kEps));
        }
        if (llSalient - llBackground < penalty) {
            params.phi[j] = 0.0;
        }
    }
}

// --- Task 4: convergence check + EM loop orchestration (spec §5) ---

// Ported verbatim from student_t_hmm.py's own _relative_delta/_check_
// convergence (the exact criterion, not the code -- that's Python/numba-
// specific): one-sided relative log-likelihood plateau, matching the
// ascent-guaranteed EM theory (McLachlan & Krishnan; Wu 1983) this repo's
// own production code already cites. A negative delta, however small, is
// never treated as convergence.
//
// This IS the EM loop's own convergence gate again (superseded a brief
// MML-in-the-loop design, see ApplyHardSaliencyPruning's own comment for
// why): since the hard saliency-pruning gate is now applied ONCE, after the
// loop converges -- not every M-step -- the loop below is plain, textbook
// EM with its full log-likelihood ascent guarantee intact, no
// objective-function mismatch to work around.
inline double RelativeDelta(double currentVal, double prevVal) {
    return (currentVal - prevVal) / std::max(std::fabs(prevVal), 1e-12);
}

inline bool CheckConvergence(
    double currentVal, bool hasPrevVal, double prevVal, double tol, int iterCount, int minIter) {
    if (!hasPrevVal || iterCount < minIter) return false;
    const double relDelta = RelativeDelta(currentVal, prevVal);
    return relDelta >= 0.0 && relDelta < tol;
}

// Observed-data log-likelihood: sum_i log( sum_k pi_k * p(x_i|k) ) (standard
// mixture log-likelihood; no MAP priors here per spec §2.5).
template <std::size_t D>
double ComputeLogLikelihood(
    const std::vector<Observation<D>>& observations, const FitParams<D>& params) {
    constexpr double kEps = 1e-300;
    const std::size_t K = params.K();
    double ll = 0.0;
    for (const auto& x : observations) {
        double total = 0.0;
        for (std::size_t k = 0; k < K; ++k) total += params.pi[k] * ComponentDensity(x, k, params);
        ll += std::log(std::max(total, kEps));
    }
    return ll;
}

template <std::size_t D>
struct FitResult {
    FitParams<D> params;
    double logLikelihood = 0.0;
    int iterations = 0;
    std::vector<double> logLikelihoodHistory;  // one entry per completed iteration, for
                                                // monotonic-ascent verification (Task 4 test)
};

// Full EM loop: k-means++ init (Task 1), phi_j=1.0 initially (spec §5), then
// alternating E-step (Task 2)/M-step (Task 3) until convergence (this
// section) or maxIterations is hit.
template <std::size_t D>
FitResult<D> FitFeatureSaliencyEM(
    const std::vector<Observation<D>>& observations, std::size_t K,
    int maxIterations, double tol, std::uint64_t rngSeed, int minIter = 1) {
    FitResult<D> result;
    if (observations.empty() || K == 0) return result;

    // Init: k-means++ seed means; every component/background variance starts
    // at the whole dataset's own per-feature variance (a standard "start
    // broad" GMM init heuristic -- not specified by the spec, a reasonable
    // engineering default, not itself a tuned/validated choice).
    const auto seeds = KMeansPlusPlusSeed<D>(observations, K, rngSeed);
    Observation<D> globalMean{}, globalVar{};
    {
        const double n = static_cast<double>(observations.size());
        for (const auto& x : observations) {
            for (std::size_t j = 0; j < D; ++j) globalMean[j] += x[j] / n;
        }
        for (const auto& x : observations) {
            for (std::size_t j = 0; j < D; ++j) {
                const double diff = x[j] - globalMean[j];
                globalVar[j] += diff * diff / n;
            }
        }
    }

    FitParams<D> params;
    params.pi.assign(K, 1.0 / static_cast<double>(K));
    params.stateMean.assign(K, Observation<D>{});
    params.stateVar.assign(K, globalVar);
    for (std::size_t k = 0; k < K && k < seeds.size(); ++k) {
        params.stateMean[k] = observations[seeds[k]];
    }
    params.bgMean = globalMean;
    params.bgVar = globalVar;
    // Real bug found via Task 5's own degenerate-input test, not a guess:
    // initializing phi_j to EXACTLY 1.0 (as spec §5's literal text says) is a
    // mathematical fixed point of the E-step formula u=[phi*f]/[phi*f+(1-phi)*q]
    // -- at phi=1, (1-phi)=0, so u=f/f=1 identically, for EVERY observation,
    // REGARDLESS of the actual salient-vs-background density ratio. The
    // M-step's phi update (mean of w*u) then trivially recomputes back to
    // exactly 1.0 forever -- an irrelevant feature can never be discovered
    // once phi lands exactly on the boundary. Fixed by starting at 1.0-1e-3
    // (still "start assuming every feature is salient" per spec §5's own
    // stated intent, but strictly inside (0,1) so genuine likelihood
    // evidence can still move it).
    params.phi.fill(1.0 - 1e-3);

    bool hasPrevLL = false;
    double prevLL = 0.0;
    int iter = 0;
    std::vector<std::vector<double>> w;
    for (; iter < maxIterations; ++iter) {
        w = ComputeStateResponsibilities<D>(observations, params);
        const auto u = ComputeFeatureResponsibilities<D>(observations, params);
        params = ComputeMStepUpdate<D>(observations, w, u);
        const double ll = ComputeLogLikelihood<D>(observations, params);
        result.logLikelihoodHistory.push_back(ll);
        const bool converged = CheckConvergence(ll, hasPrevLL, prevLL, tol, iter + 1, minIter);
        prevLL = ll;
        hasPrevLL = true;
        if (converged) {
            ++iter;
            break;
        }
    }
    // Final state responsibilities may be stale (computed against the
    // pre-last-M-step params) if the loop exited via the iteration cap
    // rather than convergence -- recompute once against the final params so
    // the hard-prune gate below always sees a consistent (w, params) pair.
    w = ComputeStateResponsibilities<D>(observations, params);
    ApplyHardSaliencyPruning<D>(params, observations, w, globalMean, globalVar);

    result.params = params;
    result.logLikelihood = prevLL;
    result.iterations = iter;
    return result;
}

}  // namespace fsem
