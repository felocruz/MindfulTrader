#include "MindfulTrader_Precompiled.h"
#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"
#include "ImbalanceIndicatorManager.h"

/*==========================================================================*/
/*
 * scsf_ImbalanceScreen2
 *
 * Imbalance Triple Screen - IS2 (intermediate). Pure display (§1.2c, resolved
 * 2026-09-07 -- SUPERSEDES the earlier per-screen-computes-its-own-dim design):
 * never calls ImbalanceClockManager::OnTick() or ImbalanceContextManager::Update()
 * -- only ImbalanceScreen1.cpp (IS1) does, driving both the cascade and the
 * centralized computation of all 4 activity-clock dims in one deterministic
 * pass. Runs at LOW_PREC_LEVEL, guaranteed to calculate AFTER IS1's
 * STD_PREC_LEVEL Update() call has already completed this exact tick -- reads
 * are structurally fresh, not merely detectably fresh (no cross-screen
 * ordering dependency exists among IS2/IS3/the collector anymore, since none
 * of them compute anything).
 *
 * Dim placement (§1.1a, resolved 2026-09-07): recurrence_rate -> IS2, mirroring
 * recurrence_rate's historical TS2 ownership on the calendar-clock side (before
 * its 2026-08-28 activity-clock replacement).
 *
 * Also displays the Gang-MACD/Phase-Coherence indicator (§1.3a, implemented
 * 2026-09-07, ImbalanceIndicatorManager) -- IS2 is this indicator's own clock
 * too, mirroring calendar-clock MACD's TS2/"INTERM_MACD" ownership
 * (TripleScreen2.cpp). Only the general-purpose 3-state signal is shown here;
 * the divergence/TRAP sub-case is not yet wired (§1.3a).
 *
 * Reference: docs/superpowers/specs/2026-09-06-imbalance-triple-screen-architecture-spec.md
 */
SCSFExport scsf_ImbalanceScreen2(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_RecurrenceRate = sc.Subgraph[0];
    SCSubgraphRef Subgraph_GangMacdSignal = sc.Subgraph[1];
    SCSubgraphRef Subgraph_GangMacdQuality = sc.Subgraph[2];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Imbalance Screen 2 - Intermediate (Recurrence Rate / Gang-MACD)";
        sc.ValueFormat = 3;
        sc.FreeDLL = 0;
        sc.GraphRegion = 1;
        sc.AutoLoop = 1;
        sc.CalculationPrecedence = LOW_PREC_LEVEL;  // pure reader, after IS1's cascade+compute (§1.2c)

        Subgraph_RecurrenceRate.Name = "IS2 Recurrence Rate (activity-clock)";
        Subgraph_RecurrenceRate.DrawStyle = DRAWSTYLE_LINE;

        Subgraph_GangMacdSignal.Name = "IS2 Gang-MACD Signal (-1 RED / 0 BLUE / +1 GREEN)";
        Subgraph_GangMacdSignal.DrawStyle = DRAWSTYLE_LINE;

        Subgraph_GangMacdQuality.Name = "IS2 Gang-MACD Quality (|dP/d\u03c4|)";
        Subgraph_GangMacdQuality.DrawStyle = DRAWSTYLE_LINE;

        return;
    }

    // Pure display read -- recurrence_rate was already computed by IS1's Update() call.
    Subgraph_RecurrenceRate[sc.Index] =
        ImbalanceContextManager::Instance().GetDim<ImbalanceContextManager::IMBALANCE_OBS_RECURRENCE_RATE>();

    // Pure display read -- Gang-MACD/Phase-Coherence was already computed by IS1's
    // ImbalanceIndicatorManager::Update() call.
    Subgraph_GangMacdSignal[sc.Index] = static_cast<float>(
        ImbalanceIndicatorManager::Instance()
            .GetSignal<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>());
    Subgraph_GangMacdQuality[sc.Index] =
        ImbalanceIndicatorManager::Instance()
            .GetQuality<ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>();
}
