#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "IndicatorKey.h"  // IndicatorKey (ACSIL-independent; see IndicatorKey.h)

namespace mts {

enum class StorageBlock : uint8_t { Int8, Float32, NotPacked };

struct IndicatorDescriptor {
    IndicatorKey  key;
    StorageBlock  block;
    size_t        position;  // index within the block's array; 0 when block == NotPacked
};

// ============================================================================
// Task 2 audit deliverable — indicator-manager-dod-soa plan
// (.superpowers/sdd/2026-08-05-indicator-manager-dod-soa/task-2-report.md has the
// full field-by-field writeup; this header is the ground-truth table it produced.)
//
// One row per published scalar tied to a *live* MTS.Schema.IndicatorState struct
// field (../schema/mts_schema.fbs:221-285). Some keys contribute a second row
// (their companion quality/norm/correlation value) or zero rows. Keys with no
// live IndicatorState field at all (InferenceManager-owned, DailyCache-owned,
// orphaned enum sentinels never wired to an IndicatorStore slot, or fields that
// exist only on TrainingEventT and not on IndicatorState) get a single NotPacked
// row so every IndicatorKey value is accounted for exactly once per row-count.
//
// Verified directly against include/Indicator.h, include/IndicatorManager.h,
// src/IndicatorManager.cpp, src/BackTesterStudy.cpp, and
// ../schema/mts_schema.fbs during the Task 2 audit (2026-08-05). Positions are
// assignment order, not IndicatorState's own field order.
// ============================================================================
inline constexpr std::array<IndicatorDescriptor, 62> kIndicatorLayout = {{
    // -- Int8-block rows: present in MapIndicatorKeyToTrainingEvent's switch
    //    (Indicator.h:1042-1073) AND IndicatorManager::PopulateIndicatorState's
    //    switch (IndicatorManager.cpp:884-918) — both switches agree on this set. --
    { IndicatorKey::LONG_MACD,               StorageBlock::Int8, 0 },
    { IndicatorKey::LONG_FI13_SIGNAL,         StorageBlock::Int8, 1 },
    { IndicatorKey::LONG_MACD_DIVERGENCE,     StorageBlock::Int8, 2 },
    { IndicatorKey::LONG_IMP,                 StorageBlock::Int8, 3 },
    { IndicatorKey::INTERM_STOCHASTIC,        StorageBlock::Int8, 4 },
    { IndicatorKey::RASCHKE_STRATEGY_SETUP,   StorageBlock::Int8, 5 },
    { IndicatorKey::RASCHKE_TACTICAL_TRIGGER, StorageBlock::Int8, 6 },
    { IndicatorKey::RSI,                      StorageBlock::Int8, 7 },
    { IndicatorKey::INTERM_FI2_SIGNAL,        StorageBlock::Int8, 8 },
    { IndicatorKey::EMA_PROXIMITY,            StorageBlock::Int8, 9 },
    { IndicatorKey::PRICE_METRICS,            StorageBlock::Int8, 10 },
    { IndicatorKey::INTERM_MACD_DIVERGENCE,   StorageBlock::Int8, 11 },
    { IndicatorKey::INTERM_IMP,               StorageBlock::Int8, 12 },
    { IndicatorKey::INTERM_MACD,              StorageBlock::Int8, 13 },
    { IndicatorKey::STRUCTURE_TEST,           StorageBlock::Int8, 14 },
    { IndicatorKey::VOLUME_SIGNAL,            StorageBlock::Int8, 15 },
    { IndicatorKey::ATR_PROXIMITY,            StorageBlock::Int8, 16 },
    // DAILY_BIAS writes BOTH daily_bias AND daily_bias_enum from the same
    // intValue() in BOTH writer paths (Indicator.h:1060-1063 and
    // IndicatorManager.cpp:902-905) — confirmed: there is exactly one source
    // read, fanned out to two mutators at serialization time. They can never
    // diverge, so one packed slot (written to both mutators at serialization
    // time) is correct; no second row needed.
    { IndicatorKey::DAILY_BIAS,               StorageBlock::Int8, 17 },
    { IndicatorKey::KANGAROO_TAIL,            StorageBlock::Int8, 18 },
    { IndicatorKey::TURTLE_SOUP,              StorageBlock::Int8, 19 },
    { IndicatorKey::MOMENTUM_PINBALL,         StorageBlock::Int8, 20 },
    { IndicatorKey::ELDER_BREAKOUT,           StorageBlock::Int8, 21 },
    { IndicatorKey::NR7,                      StorageBlock::Int8, 22 },
    { IndicatorKey::NH_NL_SIGNAL,             StorageBlock::Int8, 23 },
    { IndicatorKey::OSCILLATOR_310,           StorageBlock::Int8, 24 },  // confirmed NO companion override at all (Indicator.h:2526-2566) — the OSCILLATOR_310 crash-fix target
    { IndicatorKey::TIME_OF_DAY,              StorageBlock::Int8, 25 },
    // impulse_run_length (IndicatorState int8 field) companion of INTERM_IMP.
    // Impulse::AddToTrainingEventFB (Indicator.h:1092-1096) unconditionally
    // writes mutate_impulse_run_length() regardless of whether the specific
    // Impulse instance's Key() is LONG_IMP or INTERM_IMP (the Impulse class is
    // instantiated for both, IndicatorManager.h:146,158) — same
    // last-write-wins pattern as the Macd/interm_macd_norm discrepancy below.
    // PopulateIndicatorState (the live path) never calls this at all today.
    // BackTesterStudy.cpp:1281 is the one place that resolves the ambiguity
    // unambiguously, by construction, in favor of INTERM_IMP only — so that's
    // the key this row anchors to. Flagged, not silently fixed.
    { IndicatorKey::INTERM_IMP,               StorageBlock::Int8, 26 },

    // -- Float32-block companion rows: an extra event.indicators->mutate_X(...)
    //    call beyond the key's own Int8 row, confirmed to target a real
    //    IndicatorState struct field (not a TrainingEventT-only field). --
    { IndicatorKey::KANGAROO_TAIL,    StorageBlock::Float32, 0 },  // kangaroo_tail_quality, Indicator.h:1434-1441
    { IndicatorKey::TURTLE_SOUP,      StorageBlock::Float32, 1 },  // turtle_soup_quality, Indicator.h:1490-1496
    { IndicatorKey::MOMENTUM_PINBALL, StorageBlock::Float32, 2 },  // momentum_pinball_quality, Indicator.h:1548-1554
    { IndicatorKey::ELDER_BREAKOUT,   StorageBlock::Float32, 3 },  // elder_breakout_quality, Indicator.h:1608-1614
    { IndicatorKey::NR7,              StorageBlock::Float32, 4 },  // nr7_quality, Indicator.h:1689-1695
    // CorrelationIndicator (direct BaseIndicator subclass, not Indicator<T>) writes
    // corr_es_zn/corr_es_dx directly by key comparison, not via the switch —
    // confirmed at Indicator.h:2662-2670.
    { IndicatorKey::CORR_ES_ZN, StorageBlock::Float32, 5 },
    { IndicatorKey::CORR_ES_DX, StorageBlock::Float32, 6 },
    // interm_fi2_norm: FI2Signal::AddToTrainingEventFB (Indicator.h:1365-1369),
    // unambiguous single-instance write; also confirmed at BackTesterStudy.cpp:1276.
    { IndicatorKey::INTERM_FI2_SIGNAL, StorageBlock::Float32, 7 },
    // interm_macd_norm: NOT written by Macd::AddToTrainingEventFB (that override
    // writes a *different*, TrainingEventT-top-level `event.interm_macd_norm`
    // field, ambiguously shared between LONG_MACD's and INTERM_MACD's Macd
    // instances — Indicator.h:1125-1128, the discrepancy flagged in the Task 2
    // brief; that top-level field is NotPacked, out of scope, unaffected by this
    // row). The IndicatorState *struct* field of the same name is instead
    // populated only by BackTesterStudy.cpp:1277, unambiguously keyed to
    // INTERM_MACD's Macd instance's ZScore(). Live path and the training switch
    // do not populate the struct field at all today — flagged as a pre-existing
    // live/training gap for a later unification pass, not fixed here.
    { IndicatorKey::INTERM_MACD, StorageBlock::Float32, 8 },
    // long_fi13_norm: FI13Signal::AddToTrainingEventFB (Indicator.h:1337-1342),
    // unambiguous single-instance write (also mirrored to a TrainingEventT
    // top-level field of the same name, harmlessly, since only one Indicator
    // instance owns LONG_FI13_SIGNAL); also confirmed at BackTesterStudy.cpp:1278.
    { IndicatorKey::LONG_FI13_SIGNAL, StorageBlock::Float32, 9 },

    // -- Confirmed NotPacked (InferenceManager-owned, out of scope per spec §2) --
    { IndicatorKey::HMM_STATE,        StorageBlock::NotPacked, 0 },
    { IndicatorKey::MARKET_CLIMATE,   StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREDICTION_STATE, StorageBlock::NotPacked, 0 },
    { IndicatorKey::UNKNOWN,          StorageBlock::NotPacked, 0 },

    // -- NotPacked: no live IndicatorState field exists for this key at all.
    //    LongMarketAction/ShortMarketAction (Indicator.h:1277-1326) have no
    //    AddToTrainingEventFB override and no matching struct field anywhere;
    //    base-class dispatch falls through MapIndicatorKeyToTrainingEvent's
    //    switch default:break — the enum value is computed but never
    //    serialized anywhere. --
    { IndicatorKey::LONG_MKT_ACTION,  StorageBlock::NotPacked, 0 },
    { IndicatorKey::SHORT_MKT_ACTION, StorageBlock::NotPacked, 0 },

    // -- NotPacked: real fields, but on TrainingEvent's TOP LEVEL
    //    (../schema/mts_schema.fbs:990-992), not on the nested IndicatorState
    //    struct. Out of scope for the live-wire packed arrays per the Task 2
    //    brief (§2). --
    { IndicatorKey::SIDE,           StorageBlock::NotPacked, 0 },  // side: int8, schema:990
    { IndicatorKey::MARKET_SYMBOL,  StorageBlock::NotPacked, 0 },  // market_symbol: int8, schema:991
    { IndicatorKey::OVERNIGHT_EXIT, StorageBlock::NotPacked, 0 },  // overnight_exit: int8, schema:992

    // -- NotPacked: HURST_EXPONENT's own IndicatorKey/HurstExponentIndicator
    //    value is never serialized to IndicatorState or TrainingEvent at all.
    //    A field named hurst_exponent DOES exist in the schema, but only inside
    //    ObservationData (schema:388) and RiskGateContext (schema:440) — both
    //    populated independently by ContextManager's own DFA computation, a
    //    completely separate pipeline from IndicatorManager/IndicatorKey. --
    { IndicatorKey::HURST_EXPONENT, StorageBlock::NotPacked, 0 },

    // -- NotPacked: orphaned enum sentinels. Grepped across the entire repo —
    //    these six values appear ONLY in their own enum declaration
    //    (Indicator.h:56-61); no IndicatorStore slot in IndicatorManager.h, no
    //    switch case, no leaf class, nothing. The real prev-high/low/day-high/
    //    day-low/4-bar-high/4-bar-low values are served through two entirely
    //    separate mechanisms instead: (a) IndicatorManager's own
    //    m_dailyCache / GetCachedPrevDayHigh()-style plain getters
    //    (IndicatorManager.h:35,60-61,201-210), and (b) ShortMarketAction's own
    //    internal PriceData struct (Indicator.h:1302-1324, filled from
    //    TripleScreen3), keyed to SHORT_MKT_ACTION — not to these six keys. --
    { IndicatorKey::PREV_HIGH_KEY,          StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREV_LOW_KEY,           StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREV_DAY_HIGH_KEY,      StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREV_DAY_LOW_KEY,       StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREV_FOUR_BAR_HIGH_KEY, StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREV_FOUR_BAR_LOW_KEY,  StorageBlock::NotPacked, 0 },

    // -- NotPacked: same orphaned-sentinel pattern as above. Declared only in
    //    the enum (Indicator.h:62-63); never instantiated in
    //    IndicatorManager.h's IndicatorStore, no switch case, no leaf class.
    //    The actual "Raschke 3-10 oscillator" values are plain float
    //    parameters computed inline in TripleScreen3.cpp (from
    //    Oscillator310::FastLine()/PrevFastLine()) and passed directly into
    //    OvernightExitIndicator::SetFromOvernightContext() (Indicator.h:2001-
    //    2010) — decoupled entirely from these two IndicatorKey values. --
    { IndicatorKey::THREE_LINE_OSCILLATOR,      StorageBlock::NotPacked, 0 },
    { IndicatorKey::THREE_LINE_OSCILLATOR_PREV, StorageBlock::NotPacked, 0 },

    // -- NotPacked: real IndicatorStore objects (CrossMarketTrend,
    //    IndicatorManager.h:187-188) and real IndicatorState struct fields
    //    (zn_trend/dx_trend: int8, schema:271-272) exist, and
    //    BackTesterStudy.cpp:1265-1266 even has working, unambiguous accessor
    //    calls for both — but PopulateIndicatorState (the live path) has an
    //    explicit deferral comment: "ZN_TREND / DX_TREND: deferred to future
    //    release... Live path now consistent: fall through to default."
    //    (IndicatorManager.cpp:914-916), and the training switch
    //    (MapIndicatorKeyToTrainingEvent) has no case for either. Marked
    //    NotPacked per this explicit, current, deliberate deferral — not a
    //    silently-discovered gap. --
    { IndicatorKey::ZN_TREND, StorageBlock::NotPacked, 0 },
    { IndicatorKey::DX_TREND, StorageBlock::NotPacked, 0 },

    // -- NotPacked: real CorrelationIndicator objects (IndicatorManager.h:191-
    //    194) and real IndicatorState float fields (schema:233-236) exist, and
    //    BackTesterStudy.cpp:1286-1289 again has working accessor calls — but
    //    CorrelationIndicator::AddToTrainingEventFB itself explicitly says
    //    (Indicator.h:2668-2669): "Derivatives (delta, accel) computed from
    //    these base correlations. Not directly exported - Python calculates
    //    them." Marked NotPacked per this explicit training-path design
    //    comment; BackTesterStudy.cpp's export is a documented discrepancy to
    //    flag (see task-2-report.md), not silently resolve, in this audit. --
    { IndicatorKey::CORR_ES_ZN_DELTA, StorageBlock::NotPacked, 0 },
    { IndicatorKey::CORR_ES_ZN_ACCEL, StorageBlock::NotPacked, 0 },
    { IndicatorKey::CORR_ES_DX_DELTA, StorageBlock::NotPacked, 0 },
    { IndicatorKey::CORR_ES_DX_ACCEL, StorageBlock::NotPacked, 0 },

    // -- NotPacked: VwapIndicator::AddToTrainingEventFB is an explicit no-op —
    //    "VWAP is for trade-execution only — no FlatBuffer schema field
    //    exists." (Indicator.h:1794-1796). Confirmed: no `vwap` field anywhere
    //    in IndicatorState or TrainingEvent. --
    { IndicatorKey::VWAP, StorageBlock::NotPacked, 0 },
}};

constexpr size_t kIndicatorLayoutCount = kIndicatorLayout.size();

// Total slots needed in each block — computed once, used to size IndicatorPackedState's arrays.
constexpr size_t CountBlock(StorageBlock target) {
    size_t maxPos = 0;
    bool any = false;
    for (const auto& d : kIndicatorLayout) {
        if (d.block == target) {
            any = true;
            if (d.position + 1 > maxPos) maxPos = d.position + 1;
        }
    }
    return any ? maxPos : 0;
}

constexpr size_t kIndicatorLayoutI8Count  = CountBlock(StorageBlock::Int8);
constexpr size_t kIndicatorLayoutF32Count = CountBlock(StorageBlock::Float32);

}  // namespace mts
