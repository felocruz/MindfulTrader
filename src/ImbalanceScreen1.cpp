#include "MindfulTrader_Precompiled.h"
#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"
#include "ImbalanceIndicatorManager.h"

/*==========================================================================*/
/*
 * scsf_ImbalanceScreen1
 *
 * Imbalance Triple Screen - IS1 (macro/highest-resolution-aggregate screen).
 *
 * Producer + centralized-compute driver (architecture spec §1.2c, resolved
 * 2026-09-07 -- SUPERSEDES the earlier per-screen-computes-its-own-dim
 * design): IS1 is BOTH the highest-precedence screen (STD_PREC_LEVEL,
 * calculates first every tick) AND the sole tick-feed producer for the
 * shared ImbalanceClockManager holder. Each tick, IS1 calls
 * ImbalanceClockManager::OnTick() (drives the full IS3->IS2->IS1 bars-of-bars
 * cascade, K2=K1=4) THEN ImbalanceContextManager::Update() (computes ALL 4
 * activity-clock dims -- hurst_exponent, recurrence_rate, skewness_idx,
 * taleb_kurtosis -- in one deterministic, sequential pass), THEN reads its
 * own already-computed dim purely for display. ImbalanceScreen2.cpp/
 * ImbalanceScreen3.cpp/ImbalanceEventDataCollectorStudy.cpp never call OnTick()
 * or Update() -- they are pure readers, guaranteed fresh by construction
 * (§1.2c: no window can exist where "some dims are this tick's, some are
 * last tick's", since all 4 are computed in one function, one call).
 *
 * Dim placement (§1.1a, resolved 2026-09-07): hurst_exponent -> IS1, mirroring
 * TripleScreen1.cpp:629's calendar-clock ownership of the same dim (the
 * calendar-clock version is itself TS1/Screen-1-owned, not just conceptually
 * macro-flavored).
 *
 * Explicitly NOT yet wired here (§1.1a "Explicitly NOT scoped"):
 * IndicatorManager/IndicatorKey packed-array output (§1.3, separate design
 * pass) and ImbalanceObservationData schema field (§1.5). This study proves
 * the producer/cascade/dim pipeline end-to-end via its own Subgraph -- the
 * smallest real starting point, not the final wiring.
 *
 * Reference: docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md
 */
SCSFExport scsf_ImbalanceScreen1(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_HurstExponent = sc.Subgraph[0];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Imbalance Screen 1 - Macro (Hurst Exponent)";
        sc.ValueFormat = 2;
        sc.FreeDLL = 0;
        sc.GraphRegion = 1;
        sc.AutoLoop = 1;
        sc.CalculationPrecedence = STD_PREC_LEVEL;  // producer + highest precedence, by design (§1.2b)

        Subgraph_HurstExponent.Name = "IS1 Hurst Exponent (activity-clock)";
        Subgraph_HurstExponent.DrawStyle = DRAWSTYLE_LINE;

        ImbalanceClockManager::Instance().Reset();
        ImbalanceClockManager::Instance().ConfigureIs3Threshold(700.0f);  // matches this repo's
        // own shipped/validated fixed-threshold fallback (tools/observation_vector/
        // imbalance_screen1_hurst_eval.cpp, imbalance_clock_manager_ratio_eval.cpp), not 50.0f
        // (ImbalanceBarEngine's own uncalibrated default).
        ImbalanceClockManager::Instance().EnableIs3AdaptiveThreshold();

        return;
    }

    // Deterministic reset on chart reload/symbol change -- same discipline as
    // ActivityClockManager::Init()/TripleScreen1.cpp's own ResetAdaptiveCalculators() call.
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0) {
        ImbalanceClockManager::Instance().Reset();
        ImbalanceClockManager::Instance().ConfigureIs3Threshold(700.0f);
        ImbalanceClockManager::Instance().EnableIs3AdaptiveThreshold();
        ImbalanceContextManager::Instance().Reset();
        ImbalanceIndicatorManager::Instance().Reset();
    }

    // IS1-exclusive producer entry point (§1.2b) -- feeds the raw tick and drives the full
    // IS3->IS2->IS1 cascade. Only this study may call OnTick().
    ImbalanceClockManager::Instance().OnTick(sc.Index,
                                             static_cast<float>(sc.AskVolume[sc.Index]),
                                             static_cast<float>(sc.BidVolume[sc.Index]),
                                             static_cast<float>(sc.Close[sc.Index]));

    // IS1-exclusive centralized-compute entry point (§1.2c) -- computes all 4 activity-clock
    // dims in one deterministic pass. Only this study may call Update().
    ImbalanceContextManager::Instance().Update();

    // IS1-exclusive indicator-compute entry point (§1.3/§1.3a) -- same cascade call site,
    // right after ImbalanceContextManager::Update() so the entropy trend it reads is fresh.
    ImbalanceIndicatorManager::Instance().Update();

    // Pure display read -- hurst_exponent was already computed by Update() above.
    Subgraph_HurstExponent[sc.Index] =
        ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_HURST_EXPONENT>();
}
