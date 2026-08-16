// PredatorContext.h — unified macro context for the Predator Decision Contract
// (docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md).
//
// Composes the existing LocalRiskContext (Gang/Taleb/Pareto/Shannon intelligence, already
// populated by ContextManager every tick) with the HMM regime-state enum (previously only
// reachable via InferenceManager::Instance().HmmState()) into a single flat, POD struct.
// Composition-by-value costs nothing for cache locality — a nested POD struct lays out
// contiguously in memory identically to a flattened one, so this gives call sites one
// object instead of two separate singleton lookups, with zero DOD penalty.
//
// Every fusion free function in PredatorFusion.h takes this by const-reference — no
// allocation, no indirection, no virtual dispatch.
//
// Includes the ACSIL-independent extracted headers (LocalRiskContext.h, RcEnums.h),
// not ContextManager.h/Indicator.h directly, so this header — and everything that composes
// it (PredatorFusion.h, its native tests) — stays includable with just `-I include`, no
// sierrachart.h on the path (same convention as IndicatorComputations.h).

#pragma once

#include "LocalRiskContext.h"
#include "RcEnums.h"

struct PredatorContext {
    LocalRiskContext gang{};
    HMMStateEnum regime = HMMStateEnum::COILED_SPRING;
    bool inPosition = false;
    uint64_t applicabilityMask = 0ULL;
};
