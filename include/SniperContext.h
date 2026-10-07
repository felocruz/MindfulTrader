// SniperContext.h — unified exit-monitoring context for the Sniper Decision Contract
// (docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md §3).
//
// Mirrors PredatorContext.h's own shape exactly: a flat, POD composition of already-computed
// state (LocalRiskContext, HMMStateEnum, position state) plus one new field specific to the
// Sniper's own job -- is3BarClosed, the IS3 (Imbalance Triple Screen, finest resolution)
// bar-close cadence gate. Where the Predator surveys regime on its own patient cadence, the
// Sniper's WHERE/WHEN (stop/target placement, exit monitoring) is meant to react at the
// market's own information rate -- IS3 bar closes, not TS3's fixed 15-minute calendar closes.
//
// Composition-by-value, zero heap allocation, same reasoning as PredatorContext.h: a nested
// POD struct costs nothing for cache locality over two separate singleton lookups.
//
// Includes the ACSIL-independent extracted headers (LocalRiskContext.h, rc_enums.h), not
// ContextManager.h/Indicator.h/ImbalanceClockManager.h directly, so this header stays
// includable with just `-I include`, no sierrachart.h on the path (same convention as
// PredatorContext.h/IndicatorComputations.h).
//
// NOT YET WIRED: no ContextManager::GetSniperContext() populator exists yet (the Predator
// equivalent is ContextManager::GetPredatorContext(), src/ContextManager.cpp). This header is
// step 1 (scaffold the shape) of wiring IS3-bar-close exit monitoring -- deliberately scoped
// small, not bundled with the populator or any SniperFusion.h dispatch layer yet.

#pragma once

#include "LocalRiskContext.h"
#include "rc_enums.h"

struct SniperContext {
    LocalRiskContext gang{};
    HMMStateEnum regime = HMMStateEnum::COILED_SPRING;
    bool inPosition = false;
    // True only on the tick an IS3 bar just completed (ImbalanceClockManager::
    // GetIs3CompletedBarCount() advanced) -- the Sniper's own exit-monitoring cadence gate,
    // replacing TS3's calendar-clock bar-close as the "should we re-check exits now" signal.
    bool is3BarClosed = false;
};
