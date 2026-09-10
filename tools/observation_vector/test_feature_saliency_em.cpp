// test_feature_saliency_em.cpp — native tests for the Feature Saliency EM
// fitter (docs/superpowers/plans/2026-09-09-feature-saliency-em-fitter-
// implementation.md). No GoogleTest/CMake, check()/g_failures/ALL PASS
// harness per this repo's own convention.
//
// Build & run: g++ -std=c++17 -Iinclude \
//   tools/observation_vector/test_feature_saliency_em.cpp \
//   -o /tmp/fsem_test && /tmp/fsem_test

#include "FeatureSaliencyEM.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) std::printf("  PASS  %s\n", name);
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    // Task 1: k-means++ seeding on a well-separated 2-cluster 1D dataset --
    // assert the K=2 seeds picked are one from each cluster, across several
    // repeated deterministic draws (different rngSeed each time).
    {
        std::vector<fsem::Observation<1>> obs;
        for (int i = 0; i < 20; ++i) obs.push_back({-5.0 + 0.1 * i});   // cluster A: [-5, -3)
        for (int i = 0; i < 20; ++i) obs.push_back({5.0 + 0.1 * i});    // cluster B: [5, 7)

        bool allDrawsPickedBothClusters = true;
        for (std::uint64_t seed = 0; seed < 10; ++seed) {
            const auto seeds = fsem::KMeansPlusPlusSeed<1>(obs, 2, seed);
            if (seeds.size() != 2) { allDrawsPickedBothClusters = false; break; }
            const bool s0IsA = obs[seeds[0]][0] < 0.0;
            const bool s1IsA = obs[seeds[1]][0] < 0.0;
            if (s0IsA == s1IsA) { allDrawsPickedBothClusters = false; break; }  // both from the same cluster
        }
        check("kmeans_pp_picks_one_seed_per_cluster_across_repeated_draws", allDrawsPickedBothClusters);
    }
    {
        // Degenerate: K=0 or empty input returns no seeds, doesn't crash.
        std::vector<fsem::Observation<1>> empty;
        check("kmeans_pp_empty_input_returns_no_seeds",
              fsem::KMeansPlusPlusSeed<1>(empty, 2, 0).empty());
        std::vector<fsem::Observation<1>> obs = {{1.0}, {2.0}};
        check("kmeans_pp_k_zero_returns_no_seeds",
              fsem::KMeansPlusPlusSeed<1>(obs, 0, 0).empty());
    }

    // Task 2: E-step, hand-computable D=1/K=2 fixture (spec §2.2). State0
    // mean=0/var=1 (narrow), state1 mean=4/var=25 (wide/diffuse), background
    // mean=4/var=1 (narrow, centered AT state1's own mean -- deliberately
    // makes background density exceed state1's own salient density there).
    // phi_0=0.6, pi=[0.5,0.5]. Hand-derived (see plan/session notes):
    //   obs A (x=0): w_A0=0.87301 (clearly state0), u_{A,0,0}=0.999776 (clearly salient)
    //   obs B (x=4): w_B1=0.56514 (state1, not overwhelmingly), u_{B,1,0}=0.230784 (background-leaning)
    {
        fsem::FitParams<1> params;
        params.pi = {0.5, 0.5};
        params.stateMean = {{0.0}, {4.0}};
        params.stateVar = {{1.0}, {25.0}};
        params.bgMean = {4.0};
        params.bgVar = {1.0};
        params.phi = {0.6};

        std::vector<fsem::Observation<1>> obs = {{0.0}, {4.0}};
        const auto w = fsem::ComputeStateResponsibilities<1>(obs, params);
        const auto u = fsem::ComputeFeatureResponsibilities<1>(obs, params);

        constexpr double kEps = 1e-3;
        check("e_step_w_obsA_state0_matches_hand_computation",
              std::fabs(w[0][0] - 0.87301) < kEps);
        check("e_step_u_obsA_state0_feature0_matches_hand_computation",
              std::fabs(u[0][0][0] - 0.999776) < kEps);
        check("e_step_w_obsB_state1_matches_hand_computation",
              std::fabs(w[1][1] - 0.56514) < kEps);
        check("e_step_u_obsB_state1_feature0_matches_hand_computation",
              std::fabs(u[1][1][0] - 0.230784) < kEps);
        check("e_step_w_rows_sum_to_one",
              std::fabs(w[0][0] + w[0][1] - 1.0) < 1e-9 &&
              std::fabs(w[1][0] + w[1][1] - 1.0) < 1e-9);
    }

    // Task 3: M-step, hand-computable D=1/K=2/N=4 fixture (spec §2.3). Hard
    // state assignment (w=1.0/0.0) so obs {1,2} are pure state0 and {3,4}
    // pure state1; uniform u=0.5 everywhere a state is assigned (so both the
    // state-specific and background weighted stats are non-degenerate).
    // Hand-derived: mu_{0,0}=1.5, var_{0,0}=0.25, mu_{1,0}=3.5, var_{1,0}=0.25
    // (both states' internal spread is identical by symmetric construction);
    // background pools all 4 points with equal 0.5 weight -> mu_bg=2.5 (the
    // grand mean), var_bg=1.25; phi_0=0.5 (uniform salient/background split);
    // pi=[0.5,0.5] (two observations per state).
    {
        std::vector<fsem::Observation<1>> obs = {{1.0}, {2.0}, {3.0}, {4.0}};
        std::vector<std::vector<double>> w = {
            {1.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 1.0}};
        std::vector<std::vector<fsem::Observation<1>>> u = {
            {{0.5}, {0.5}}, {{0.5}, {0.5}}, {{0.5}, {0.5}}, {{0.5}, {0.5}}};

        const auto params = fsem::ComputeMStepUpdate<1>(obs, w, u);
        constexpr double kEps = 1e-9;
        check("m_step_state0_mean_matches_hand_computation",
              std::fabs(params.stateMean[0][0] - 1.5) < kEps);
        check("m_step_state0_var_matches_hand_computation",
              std::fabs(params.stateVar[0][0] - 0.25) < kEps);
        check("m_step_state1_mean_matches_hand_computation",
              std::fabs(params.stateMean[1][0] - 3.5) < kEps);
        check("m_step_state1_var_matches_hand_computation",
              std::fabs(params.stateVar[1][0] - 0.25) < kEps);
        check("m_step_background_mean_matches_hand_computation",
              std::fabs(params.bgMean[0] - 2.5) < kEps);
        check("m_step_background_var_matches_hand_computation",
              std::fabs(params.bgVar[0] - 1.25) < kEps);
        check("m_step_phi_matches_hand_computation",
              std::fabs(params.phi[0] - 0.5) < kEps);
        check("m_step_pi_matches_hand_computation",
              std::fabs(params.pi[0] - 0.5) < kEps && std::fabs(params.pi[1] - 0.5) < kEps);
    }

    // Task 4: full EM loop on a trivial, obviously-separated 2-cluster 1D
    // fixture -- assert log-likelihood is monotonically non-decreasing
    // across every completed iteration (the ascent-guarantee property) and
    // that the loop converges before hitting the iteration cap.
    {
        std::vector<fsem::Observation<1>> obs;
        uint32_t seed = 42;
        auto nextJitter = [&seed]() -> double {
            seed = seed * 1664525u + 1013904223u;
            return (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;  // [-1,1]
        };
        for (int i = 0; i < 30; ++i) obs.push_back({-5.0 + 0.3 * nextJitter()});
        for (int i = 0; i < 30; ++i) obs.push_back({5.0 + 0.3 * nextJitter()});

        constexpr int kMaxIterations = 50;
        const auto result = fsem::FitFeatureSaliencyEM<1>(obs, 2, kMaxIterations, 1e-6, /*rngSeed=*/7);

        check("em_loop_converges_before_iteration_cap", result.iterations < kMaxIterations);
        check("em_loop_ran_at_least_one_iteration", result.iterations > 0);

        bool monotonicNonDecreasing = true;
        for (std::size_t i = 1; i < result.logLikelihoodHistory.size(); ++i) {
            // Tiny numerical slack for floating-point noise, not a real regression.
            if (result.logLikelihoodHistory[i] < result.logLikelihoodHistory[i - 1] - 1e-9) {
                monotonicNonDecreasing = false;
                break;
            }
        }
        check("em_loop_log_likelihood_monotonically_non_decreasing", monotonicNonDecreasing);
    }

    // Task 5: degenerate-input tests (D=2, K=2) -- feature 0 genuinely
    // separates the two clusters (planted salient), feature 1 is drawn from
    // the SAME distribution regardless of which cluster the observation
    // belongs to (planted irrelevant/background-only). Should pass against
    // Task 4's fitter with no new code, per the task's own framing.
    {
        std::vector<fsem::Observation<2>> obs;
        uint32_t seed = 999;
        auto nextJitter = [&seed]() -> double {
            seed = seed * 1664525u + 1013904223u;
            return (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;  // [-1,1]
        };
        for (int i = 0; i < 40; ++i) {
            obs.push_back({-5.0 + 0.3 * nextJitter(), 0.0 + 1.0 * nextJitter()});
        }
        for (int i = 0; i < 40; ++i) {
            obs.push_back({5.0 + 0.3 * nextJitter(), 0.0 + 1.0 * nextJitter()});
        }

        const auto result = fsem::FitFeatureSaliencyEM<2>(obs, 2, 50, 1e-6, /*rngSeed=*/13);
        check("degenerate_planted_salient_feature_converges_phi_near_one",
              result.params.phi[0] > 0.8);
        check("degenerate_planted_irrelevant_feature_converges_phi_near_zero",
              result.params.phi[1] < 0.2);
    }

    // Task 6: synthetic recovery test -- THE correctness proof (spec §6, item
    // 1). K=4 known components, D=8 features: dims 0-3 planted salient (a
    // distinct +-5 sign pattern per state across all 4 salient dims
    // simultaneously -- clean multi-dimensional cluster separation, not just
    // a single discriminating dim), dims 4-7 planted irrelevant (N(0,1)
    // regardless of assigned state). Fixed-seed deterministic RNG.
    {
        constexpr std::size_t kK = 4;
        constexpr std::size_t kD = 8;
        // Each row is one state's sign pattern across the 4 salient dims --
        // pairwise Euclidean separation (>=14 at amplitude 5, jitter +-0.3)
        // keeps k-means++ init away from the label-swap local optima Task 5's
        // own robustness sweep found at smaller K/D.
        const double pattern[kK][4] = {
            { 1.0,  1.0,  1.0,  1.0},
            { 1.0, -1.0,  1.0, -1.0},
            {-1.0,  1.0, -1.0,  1.0},
            {-1.0, -1.0, -1.0, -1.0},
        };
        constexpr int kNPerState = 100;
        std::vector<fsem::Observation<kD>> obs;
        uint32_t seed = 2026;
        auto nextJitter = [&seed]() -> double {
            seed = seed * 1664525u + 1013904223u;
            return (static_cast<double>(seed % 2001) - 1000.0) / 1000.0;  // [-1,1]
        };
        for (std::size_t k = 0; k < kK; ++k) {
            for (int i = 0; i < kNPerState; ++i) {
                fsem::Observation<kD> x{};
                for (std::size_t j = 0; j < 4; ++j) x[j] = pattern[k][j] * 5.0 + 0.3 * nextJitter();
                for (std::size_t j = 4; j < kD; ++j) x[j] = 0.0 + 1.0 * nextJitter();
                obs.push_back(x);
            }
        }

        const auto result = fsem::FitFeatureSaliencyEM<kD>(obs, kK, 300, 1e-6, /*rngSeed=*/13);
        bool allSalientHigh = true;
        bool allIrrelevantLow = true;
        for (std::size_t j = 0; j < 4; ++j) {
            if (result.params.phi[j] <= 0.8) allSalientHigh = false;
        }
        for (std::size_t j = 4; j < kD; ++j) {
            if (result.params.phi[j] >= 0.2) allIrrelevantLow = false;
        }
        check("synthetic_recovery_planted_salient_features_phi_above_0_8", allSalientHigh);
        check("synthetic_recovery_planted_irrelevant_features_phi_below_0_2", allIrrelevantLow);
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
