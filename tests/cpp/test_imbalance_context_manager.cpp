// test_imbalance_context_manager.cpp -- unit tests for ImbalanceContextManager's
// centralized dim computation (architecture spec §1.2c/§1.2d, 2026-09-07):
// proves Update() computes all 4 activity-clock dims (hurst_exponent,
// recurrence_rate, skewness_idx, taleb_kurtosis) in one deterministic pass,
// AllDimsReady() simplifies to "has Update() ever been called", and carry-
// forward (NaN on a degenerate/insufficient-data window -> reuse last valid
// value) is applied identically across all 4 dims.
//
// ImbalanceClockManager/ImbalanceContextManager are both singletons
// (Instance()-only) -- every test block resets BOTH at the start.
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_imbalance_context_manager.cpp -o /tmp/icxm_test && /tmp/icxm_test

#include "ImbalanceContextManager.h"
#include "ImbalanceClockManager.h"
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

void ResetBoth() {
    ImbalanceClockManager::Instance().Reset();
    ImbalanceContextManager::Instance().Reset();
}

// Feeds `count` synthetic ticks through ImbalanceClockManager, each one large
// enough (in ask/bid volume imbalance) to close an IS3 bar -- the cheapest way
// to get real, non-degenerate IS1/IS2/IS3 return series without needing real
// tick data, since ImbalanceBarEngine's own adaptive threshold starts low.
void FeedSyntheticTicks(int count, float startPrice = 100.0f) {
    float price = startPrice;
    for (int i = 0; i < count; ++i) {
        price += ((i % 2 == 0) ? 0.25f : -0.15f);  // non-degenerate up/down walk
        ImbalanceClockManager::Instance().OnTick(i, 50.0f, 1.0f, price);
    }
}
}  // namespace

int main() {
    {
        ResetBoth();
        check("fresh state: AllDimsReady() is false before Update() is ever called",
              !ImbalanceContextManager::Instance().AllDimsReady());
    }

    {
        ResetBoth();
        ImbalanceContextManager::Instance().Update();
        check("AllDimsReady() is true after a single Update() call, even with zero real data "
              "(freshness is structural, not per-dim-tracked -- §1.2c)",
              ImbalanceContextManager::Instance().AllDimsReady());
    }

    {
        // Carry-forward: with no real returns fed in, every dim's compute function returns
        // NaN, so Update() must carry forward the neutral seed defaults untouched.
        ResetBoth();
        ImbalanceContextManager::Instance().Update();
        const auto obs = ImbalanceContextManager::Instance().BuildObservation();
        check("carry-forward on insufficient data: hurst_exponent stays at its neutral seed (0.5)",
              obs.hurst_exponent() == 0.5f);
        check("carry-forward on insufficient data: recurrence_rate stays at its neutral seed (0.0)",
              obs.recurrence_rate() == 0.0f);
        check("carry-forward on insufficient data: skewness_idx stays at its neutral seed (0.0)",
              obs.skewness_idx() == 0.0f);
        check("carry-forward on insufficient data: taleb_kurtosis stays at its neutral seed (1.23)",
              obs.taleb_kurtosis() == 1.23f);
    }

    {
        // Real data: feed enough synthetic ticks to close >= 100 IS3 bars (and therefore
        // >= 25 IS2 bars and >= 6 IS1 bars at K2=K1=4) -- Update() should now produce
        // finite, non-default values for all 4 dims.
        ResetBoth();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        const auto obs = ImbalanceContextManager::Instance().BuildObservation();
        check("with real IS3 data, skewness_idx() is finite",
              std::isfinite(obs.skewness_idx()));
        check("with real IS3 data, taleb_kurtosis() is finite",
              std::isfinite(obs.taleb_kurtosis()));
        check("GetDim<Index>() matches BuildObservation()'s own values",
              ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_SKEWNESS_IDX>() ==
                  obs.skewness_idx());
    }

    {
        // recurrence_rate's own dedicated cache: calling Update() again with no new IS2 bar
        // closed must NOT recompute (NaN from ComputeImbalanceRecurrenceRate) -- but the
        // generic carry-forward means the previously-computed value is still reported,
        // not reset to the neutral seed.
        ResetBoth();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        const float firstRecurrence =
            ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_RECURRENCE_RATE>();
        ImbalanceContextManager::Instance().Update();  // no new ticks fed -- no new IS2 bar
        const float secondRecurrence =
            ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_RECURRENCE_RATE>();
        check("recurrence_rate is carried forward unchanged across an Update() call with no new IS2 bar",
              firstRecurrence == secondRecurrence);
    }

    {
        ResetBoth();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        check("sanity: ready before Reset()", ImbalanceContextManager::Instance().AllDimsReady());

        ImbalanceContextManager::Instance().Reset();
        check("Reset() clears readiness", !ImbalanceContextManager::Instance().AllDimsReady());

        const auto obs = ImbalanceContextManager::Instance().BuildObservation();
        check("Reset() restores neutral seed defaults (hurst=0.5, taleb_kurtosis=1.23)",
              obs.hurst_exponent() == 0.5f && obs.taleb_kurtosis() == 1.23f);
    }

    {
        // shannon_flow_entropy/shannon_efficiency (§1.3b): with no real data, entropy stays at
        // its neutral cold-start seed (0 bits entropy -> 0.5 efficiency).
        ResetBoth();
        ImbalanceContextManager::Instance().Update();
        check("cold start: shannon_flow_entropy is 0.0 bits",
              ImbalanceContextManager::Instance().GetShannonFlowEntropy() == 0.0f);
        check("cold start: shannon_efficiency is the neutral 0.5 seed",
              ImbalanceContextManager::Instance().GetShannonEfficiency() == 0.5f);
    }

    {
        // With real IS2 bar closes fed in, entropy should move off its cold-start seed and
        // stay within its documented [0, log2(10)] bits / [0,1] efficiency bounds.
        ResetBoth();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        const float entropy = ImbalanceContextManager::Instance().GetShannonFlowEntropy();
        const float efficiency = ImbalanceContextManager::Instance().GetShannonEfficiency();
        check("with real IS2 data, shannon_flow_entropy is finite and within [0, log2(10)] bits",
              std::isfinite(entropy) && entropy >= 0.0f && entropy <= 3.321928f);
        check("with real IS2 data, shannon_efficiency is finite and within [0,1]",
              std::isfinite(efficiency) && efficiency >= 0.0f && efficiency <= 1.0f);
    }

    {
        // BuildRiskGateContext() carries the exact same values as the observation vector +
        // entropy accessors -- no independent/duplicated computation.
        ResetBoth();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        const auto riskGateCtx = ImbalanceContextManager::Instance().BuildRiskGateContext(12345);
        check("BuildRiskGateContext(): shannon_flow_entropy matches GetShannonFlowEntropy()",
              riskGateCtx.shannon_flow_entropy == ImbalanceContextManager::Instance().GetShannonFlowEntropy());
        check("BuildRiskGateContext(): hurst_exponent matches the observation vector's own value",
              riskGateCtx.hurst_exponent ==
                  ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_HURST_EXPONENT>());
        check("BuildRiskGateContext(): is_valid is true once Update() has been called",
              riskGateCtx.is_valid == true);
        check("BuildRiskGateContext(): snapshot_timestamp_us carries the caller's timestamp",
              riskGateCtx.snapshot_timestamp_us == 12345);
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}

