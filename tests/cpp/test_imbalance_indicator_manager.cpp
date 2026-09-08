// test_imbalance_indicator_manager.cpp -- unit tests for
// ImbalanceIndicatorManager's Gang-MACD/Phase-Coherence indicator
// (architecture spec §1.3/§1.3a, 2026-09-07): the pure ComputeGangMacdPhaseCoherence()
// classification function, and the Update() orchestrator's entropy-trend/
// phase-velocity integration.
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_imbalance_indicator_manager.cpp -o /tmp/iim_test && /tmp/iim_test

#include "ImbalanceIndicatorManager.h"
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

void ResetAll() {
    ImbalanceClockManager::Instance().Reset();
    ImbalanceContextManager::Instance().Reset();
    ImbalanceIndicatorManager::Instance().Reset();
}

void FeedSyntheticTicks(int count, float startPrice = 100.0f) {
    float price = startPrice;
    for (int i = 0; i < count; ++i) {
        price += ((i % 2 == 0) ? 0.25f : -0.15f);
        ImbalanceClockManager::Instance().OnTick(i, 50.0f, 1.0f, price);
    }
}
}  // namespace

int main() {
    // --- Pure function: ComputeGangMacdPhaseCoherence() ---
    {
        const auto r = ComputeGangMacdPhaseCoherence(/*phaseVelocity=*/0.5f, /*entropyTrend=*/0.1f);
        check("entropy rising (>0): BLUE regardless of phase velocity sign",
              r.signal == static_cast<int8_t>(GangMacdSignal::BLUE));
        check("quality is |phaseVelocity| even on the BLUE branch", r.quality == 0.5f);
    }
    {
        const auto r = ComputeGangMacdPhaseCoherence(0.5f, -0.1f);
        check("entropy falling (<0), price rising: GREEN",
              r.signal == static_cast<int8_t>(GangMacdSignal::GREEN));
        check("quality is |phaseVelocity|", r.quality == 0.5f);
    }
    {
        const auto r = ComputeGangMacdPhaseCoherence(-0.5f, -0.1f);
        check("entropy falling (<0), price falling: RED",
              r.signal == static_cast<int8_t>(GangMacdSignal::RED));
        check("quality is |phaseVelocity| (always non-negative)", r.quality == 0.5f);
    }
    {
        const auto r = ComputeGangMacdPhaseCoherence(0.0f, 0.0f);
        check("entropy flat (==0), price flat (==0): BLUE (no direction to be coherent about)",
              r.signal == static_cast<int8_t>(GangMacdSignal::BLUE));
        check("quality is 0.0 when phase velocity is 0.0", r.quality == 0.0f);
    }
    {
        const auto r = ComputeGangMacdPhaseCoherence(0.5f, 0.0f);
        check("entropy exactly 0 (<=0 branch), price rising: GREEN, not BLUE",
              r.signal == static_cast<int8_t>(GangMacdSignal::GREEN));
    }

    // --- Update() orchestrator ---
    {
        ResetAll();
        check("fresh state: signal defaults to BLUE (neutral)",
              ImbalanceIndicatorManager::Instance()
                      .GetSignal<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>() ==
                  static_cast<int8_t>(GangMacdSignal::BLUE));
        check("fresh state: quality defaults to 0.0",
              ImbalanceIndicatorManager::Instance()
                      .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>() == 0.0f);
    }

    {
        // With real data, Update() must not crash and must report a finite quality --
        // exact signal value depends on the synthetic walk's own entropy/return dynamics,
        // not asserted here (that's ComputeGangMacdPhaseCoherence()'s own job above).
        ResetAll();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();  // populates the entropy engine IS2 reads
        ImbalanceIndicatorManager::Instance().Update();
        const float quality = ImbalanceIndicatorManager::Instance()
            .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>();
        check("with real IS2 data, quality is finite and non-negative",
              std::isfinite(quality) && quality >= 0.0f);
    }

    {
        // Calling Update() again with no new ticks/bars must not corrupt state (idempotent
        // given unchanged inputs) -- entropyTrend collapses to 0.0 (no new bar), quality stays
        // pinned to the same |phaseVelocity| since GetIs2Returns(1,...) is non-destructive.
        ResetAll();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        ImbalanceIndicatorManager::Instance().Update();
        const float firstQuality = ImbalanceIndicatorManager::Instance()
            .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>();

        ImbalanceIndicatorManager::Instance().Update();  // no new ticks fed
        const float secondQuality = ImbalanceIndicatorManager::Instance()
            .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>();
        check("quality is stable across an Update() call with no new IS2 bar",
              firstQuality == secondQuality);
    }

    {
        ResetAll();
        FeedSyntheticTicks(2000);
        ImbalanceContextManager::Instance().Update();
        ImbalanceIndicatorManager::Instance().Update();
        ImbalanceIndicatorManager::Instance().Reset();
        check("Reset() restores the neutral BLUE/0.0 defaults",
              ImbalanceIndicatorManager::Instance()
                      .GetSignal<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>() ==
                  static_cast<int8_t>(GangMacdSignal::BLUE) &&
              ImbalanceIndicatorManager::Instance()
                      .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>() == 0.0f);
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
