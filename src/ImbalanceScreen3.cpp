#include "MindfulTrader_Precompiled.h"
#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"

/*==========================================================================*/
/*
 * scsf_ImbalanceScreen3
 *
 * Imbalance Triple Screen - IS3 (micro, finest resolution -- the one screen
 * whose bars are real adaptive threshold-crossing bars, not aggregates). Pure
 * display (§1.2c, resolved 2026-09-07 -- SUPERSEDES the earlier per-screen-
 * computes-its-own-dim design): never calls ImbalanceClockManager::OnTick()
 * or ImbalanceContextManager::Update() -- only ImbalanceScreen1.cpp (IS1) does,
 * driving both the cascade and the centralized computation of all 4
 * activity-clock dims in one deterministic pass. Runs at LOW_PREC_LEVEL
 * (COLLAPSED 2026-09-07 from the earlier VERY_LOW_PREC_LEVEL split: the
 * precedence-tier problem this split guarded against -- IS3 needing to
 * cross-read IS2's computed dim -- dissolves under centralized computation,
 * since ImbalanceContextManager's own internal computation order already
 * computes IS2's recurrence_rate before IS3's skewness/kurtosis; a future IS3
 * dim can simply read the already-computed value from the same struct it's
 * about to write into, no precedence-tier coordination ever required).
 *
 * Dim placement (§1.1a, resolved 2026-09-07): skewness_idx + taleb_kurtosis ->
 * IS3, mirroring both dims' calendar-clock ownership (TripleScreen3.cpp's own
 * "SCREEN 3 (RIPPLE)" section, TripleScreen3.cpp:788-793/1381) -- both are
 * TS3/Ripple-owned "shape of the return distribution, fast-reacting" measures
 * on the calendar-clock side, a real grouping, not coincidental.
 *
 * Reference: docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md
 */
SCSFExport scsf_ImbalanceScreen3(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_SkewnessIdx = sc.Subgraph[0];
    SCSubgraphRef Subgraph_TalebKurtosis = sc.Subgraph[1];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Imbalance Screen 3 - Micro (Skewness / Taleb Kurtosis)";
        sc.ValueFormat = 3;
        sc.FreeDLL = 0;
        sc.GraphRegion = 1;
        sc.AutoLoop = 1;
        sc.CalculationPrecedence = LOW_PREC_LEVEL;  // pure reader, after IS1's cascade+compute (§1.2c)

        Subgraph_SkewnessIdx.Name = "IS3 Skewness Idx (activity-clock)";
        Subgraph_SkewnessIdx.DrawStyle = DRAWSTYLE_LINE;

        Subgraph_TalebKurtosis.Name = "IS3 Taleb Kurtosis (activity-clock)";
        Subgraph_TalebKurtosis.DrawStyle = DRAWSTYLE_LINE;

        return;
    }

    // Pure display read -- skewness_idx/taleb_kurtosis were already computed by IS1's
    // Update() call.
    Subgraph_SkewnessIdx[sc.Index] =
        ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_SKEWNESS_IDX>();
    Subgraph_TalebKurtosis[sc.Index] =
        ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_TALEB_KURTOSIS>();
}
