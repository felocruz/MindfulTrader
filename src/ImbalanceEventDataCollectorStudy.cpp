#include "MindfulTrader_Precompiled.h"
#include "ImbalanceContextManager.h"
#include "LBRFileManager.h"

/*==========================================================================*/
/*
 * scsf_ImbalanceEventDataCollectorStudy
 *
 * Mirrors EventDataCollectorStudy.cpp's role/separation exactly (architecture
 * spec §1.6): a SEPARATE, optional ACSIL study whose only job is assembling +
 * writing the .imbalance.context stream. ImbalanceScreen1/2/3.cpp stay fully
 * agnostic to whether a collection run is active -- they only compute and
 * expose their own dim via ImbalanceContextManager, never touch LBRFileManager
 * themselves (2026-09-07 correction: an earlier version of this pipeline had
 * ImbalanceScreen3.cpp trigger the write directly, coupling a "screen" study
 * to a "data collection" concern; moved here instead).
 *
 * Precedence coordination (§1.2c, resolved 2026-09-07): IS1 = STD_PREC_LEVEL
 * (producer + centralized-compute driver, via ImbalanceContextManager::Update()),
 * IS2/IS3 = LOW_PREC_LEVEL (pure readers, no ordering dependency between them --
 * computation happens once, centrally, in IS1's own tick handler, before either
 * runs). This collector = VERY_LOW_PREC_LEVEL, kept defensively last: §1.2c's
 * centralized computation means correctness no longer strictly requires a 4th
 * tier below IS2/IS3 (IS1's Update() call always completes before any
 * LOW_PREC_LEVEL study runs, regardless of intra-tier ordering), but there is
 * no cost to keeping the collector last anyway, and it removes any dependence
 * on Sierra Chart's own unspecified intra-tier ordering guarantees.
 *
 * AllDimsReady() is the only readiness gate in this initial slice -- a real
 * multi-lock gate (mirroring EventDataCollectorStudy.cpp's 5-lock system) is
 * separate, later work, not yet designed for the imbalance clock. It now
 * simplifies to "has ImbalanceContextManager::Update() been called at least
 * once" (§1.2c: freshness is structural, not per-dim-tracked).
 * LogImbalanceContext() itself is a safe no-op when LBRFileManager::Open() was
 * never called (no active collection run) -- no extra guard needed here.
 *
 * This file's own ultimate fate (§1.2e's open fork -- whether it still has a
 * job once ImbalanceContextManager::Update() could emit internally) remains
 * NOT YET RESOLVED, deferred until §1.3's ImbalanceIndicatorManager design
 * lands. Kept as-is here on purpose.
 *
 * Reference: docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md
 */
SCSFExport scsf_ImbalanceEventDataCollectorStudy(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "Imbalance Event Data Collector";
        sc.FreeDLL = 0;
        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.HideStudy = 1;
        sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL;  // kept defensively last (§1.2c)

        return;
    }

    if (ImbalanceContextManager::Instance().AllDimsReady()) {
        const auto obs = ImbalanceContextManager::Instance().BuildObservation();
        const uint64_t timestamp_us = static_cast<uint64_t>(sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds());
        const auto riskGateContext = ImbalanceContextManager::Instance().BuildRiskGateContext(timestamp_us);
        LBRFileManager::Instance().LogImbalanceContext(obs, timestamp_us, &riskGateContext);
    }
}
